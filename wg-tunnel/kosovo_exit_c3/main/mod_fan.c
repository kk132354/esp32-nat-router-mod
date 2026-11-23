/*
 * Temperaturgesteuerter Luefter (Transistor oder Relais) mit Webseite /fan.
 *
 * - alle FAN_CHECK_S Sekunden wird die Chip-Temperatur gemessen
 * - AUTO: ab "AN-Grenze" Luefter an, bei "AUS-Grenze" wieder aus (Hysterese),
 *   mindestens FAN_MIN_HOLD_S Sekunden zwischen zwei Schaltvorgaengen
 * - Messung faellt aus (z. B. klassischer ESP32): im AUTO-Modus laeuft der Luefter dauerhaft
 * - Webseite /fan: Modus (Auto/Immer AN/Immer AUS) und Grenzwerte aendern.
 *   Aendern: siehe web.h (Passwort/Anmeldung auf der Seite "Sicherheit").
 *   Die Grenzwerte werden im Flash gespeichert, der Modus faellt nach Neustart auf AUTO zurueck.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "module_api.h"
#include "web.h"

#if CONFIG_IDF_TARGET_ESP32
/* Der klassische ESP32 hat keinen offiziell unterstuetzten Sensor. Diese alte
 * interne Funktion liefert oft nur den festen Wert 128 (= "kein Wert"). In dem
 * Fall laeuft der Luefter im AUTO-Modus dauerhaft (sichere Rueckfallloesung). */
extern uint8_t temprature_sens_read(void);
#else
#include "driver/temperature_sensor.h"
static temperature_sensor_handle_t s_tsens;
#endif

static const char *TAG = "fan";

#ifdef CONFIG_FAN_ACTIVE_LOW
#define LEVEL_ON 0
#else
#define LEVEL_ON 1
#endif

typedef enum { MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2 } fan_mode_t;

static volatile float s_temp = -100.0f;
static volatile bool s_on = false;
static volatile int s_on_temp = CONFIG_FAN_ON_TEMP_C;
static volatile int s_off_temp = CONFIG_FAN_OFF_TEMP_C;
static volatile fan_mode_t s_mode = MODE_AUTO;

static bool limits_valid(int on, int off)
{
    return on >= 30 && on <= 90 && off >= 20 && off <= on - 2;
}

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("fan", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int8_t on, off;
    if (nvs_get_i8(h, "on", &on) == ESP_OK && nvs_get_i8(h, "off", &off) == ESP_OK &&
        limits_valid(on, off)) {
        s_on_temp = on;
        s_off_temp = off;
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("fan", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i8(h, "on", (int8_t)s_on_temp);
    nvs_set_i8(h, "off", (int8_t)s_off_temp);
    nvs_commit(h);
    nvs_close(h);
}

static void fan_set(bool on)
{
    gpio_set_level(CONFIG_FAN_GPIO, on ? LEVEL_ON : !LEVEL_ON);
    s_on = on;
}

static bool read_temp(float *t)
{
#if CONFIG_IDF_TARGET_ESP32
    uint8_t raw = temprature_sens_read();
    if (raw == 128) { /* 128 = Sensor liefert nichts */
        return false;
    }
    *t = ((float)raw - 32.0f) / 1.8f;
    return true;
#else
    return temperature_sensor_get_celsius(s_tsens, t) == ESP_OK;
#endif
}

static void fan_task(void *arg)
{
    TickType_t last_switch = xTaskGetTickCount();
    int fails = 0;
    int log_counter = 0;

    for (;;) {
        float t = 0;
        bool ok = read_temp(&t);
        if (ok) {
            fails = 0;
            s_temp = t;
        } else {
            fails++;
        }

        TickType_t now = xTaskGetTickCount();
        bool hold_ok = (now - last_switch) >= pdMS_TO_TICKS(CONFIG_FAN_MIN_HOLD_S * 1000);
        fan_mode_t mode = s_mode;
        int on_t = s_on_temp;
        int off_t = s_off_temp;

        if (mode == MODE_ON) {
            if (!s_on) {
                fan_set(true);
                last_switch = now;
                ESP_LOGW(TAG, "Luefter manuell AN");
            }
        } else if (mode == MODE_OFF) {
            if (s_on) {
                fan_set(false);
                last_switch = now;
                ESP_LOGW(TAG, "Luefter manuell AUS");
            }
        } else if (ok) {
            if (!s_on && t >= on_t && hold_ok) {
                fan_set(true);
                last_switch = now;
                ESP_LOGW(TAG, "%.1f C >= %d C: Luefter AN", t, on_t);
            } else if (s_on && t <= off_t && hold_ok) {
                fan_set(false);
                last_switch = now;
                ESP_LOGI(TAG, "%.1f C <= %d C: Luefter AUS", t, off_t);
            }
        } else if (fails >= 3 && !s_on) {
            ESP_LOGE(TAG, "Temperaturmessung faellt aus, Luefter sicherheitshalber AN");
            fan_set(true);
            last_switch = now;
        }

        if (ok && ++log_counter >= 6) { /* alle ~30 s eine Zeile */
            log_counter = 0;
            ESP_LOGI(TAG, "Chip %.1f C, Luefter %s", t, s_on ? "AN" : "aus");
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FAN_CHECK_S * 1000));
    }
}

static const char *mode_text(fan_mode_t m)
{
    return m == MODE_ON ? "Immer AN" : (m == MODE_OFF ? "Immer AUS" : "Automatisch");
}

/* ---------------- Webseite /fan ---------------- */

static size_t fan_pins(module_pin_t *out, size_t max)
{
    static const module_pin_t list[] = {
        {CONFIG_FAN_GPIO, true,
#ifdef CONFIG_FAN_ACTIVE_LOW
         "Signal: ueber ca. 1 kOhm an die Basis des S8050 (Luefter AN = Pegel NIEDRIG, nur mit passender Schaltung)"},
#else
         "Signal: ueber ca. 1 kOhm an die Basis des S8050 (Luefter AN = Pegel HOCH)"},
#endif
        {-1, false, "Luefter Plus (+): an 5V vom Pico (VBUS)"},
        {-1, false, "Luefter Minus (-): an den Kollektor des S8050"},
        {-1, false, "Emitter des S8050: an GND, GND von ESP und Pico verbinden"},
        {-1, false, "Diode (z. B. 1N4148) quer zum Luefter, Strich zu +5V"},
        {-1, false, "S8050 (flache Seite zu dir, Beine nach unten): links Emitter, Mitte Basis, rechts Kollektor"},
        {-1, false, "Temperatur: interner Chip-Sensor, kein Pin noetig"},
    };
    size_t n = sizeof(list) / sizeof(list[0]);
    if (n > max) {
        n = max;
    }
    memcpy(out, list, n * sizeof(list[0]));
    return n;
}

static esp_err_t fan_page(httpd_req_t *req)
{
    const char *msg = "";

    if (req->method == HTTP_POST) {
        if (!web_can_control(req)) {
            msg = web_password_set() ? "Nicht erlaubt: bitte erst anmelden." : "Nicht erlaubt aus diesem Netz.";
        } else {
            char *body = web_read_body(req);
            char m[12] = "", a[12] = "", b[12] = "";
            bool have_m = false, have_ab = false;
            if (body) {
                have_m = web_form_get(body, "mode", m, sizeof(m));
                have_ab = web_form_get(body, "on", a, sizeof(a)) && web_form_get(body, "off", b, sizeof(b));
                free(body);
            }
            if (have_m) {
                s_mode = !strcmp(m, "on") ? MODE_ON : (!strcmp(m, "off") ? MODE_OFF : MODE_AUTO);
            }
            msg = "Gespeichert.";
            if (have_ab) {
                int on = atoi(a), off = atoi(b);
                if (limits_valid(on, off)) {
                    s_on_temp = on;
                    s_off_temp = off;
                    settings_save();
                } else {
                    msg = "Ungueltig: AN 30-90, AUS mind. 20 und mind. 2 unter AN.";
                }
            }
        }
    }

    fan_mode_t mode = s_mode;
    char pins[1100];
    modules_pins_html(pins, sizeof(pins), "fan");
    char page[2800];
    snprintf(page, sizeof(page),
        "<p><b>Temperatur:</b> %.1f C<br><b>Luefter:</b> %s<br><b>Modus:</b> %s<br>"
        "<b>AN ab</b> %d C, <b>AUS bei</b> %d C</p>"
        "<p class=msg>%s</p>"
        "<form method=post action=fan>"
        "Modus: <select name=mode>"
        "<option value=auto%s>Automatisch</option>"
        "<option value=on%s>Immer AN</option>"
        "<option value=off%s>Immer AUS</option></select><br>"
        "AN ab (C): <input name=on type=number value=%d style='width:80px'><br>"
        "AUS bei (C): <input name=off type=number value=%d style='width:80px'><br>"
        "<button type=submit>Speichern</button></form>"
        "<p><small>Zum Aendern ggf. unter &quot;Sicherheit&quot; anmelden.</small></p>%s",
        (double)s_temp, s_on ? "AN" : "aus", mode_text(mode), s_on_temp, s_off_temp, msg,
        mode == MODE_AUTO ? " selected" : "", mode == MODE_ON ? " selected" : "",
        mode == MODE_OFF ? " selected" : "", s_on_temp, s_off_temp, pins);
    return web_send_page(req, "Luefter", page);
}

static void fan_http(httpd_handle_t srv)
{
    httpd_uri_t g = {.uri = "/fan", .method = HTTP_GET, .handler = fan_page, .user_ctx = NULL};
    httpd_uri_t p = {.uri = "/fan", .method = HTTP_POST, .handler = fan_page, .user_ctx = NULL};
    httpd_register_uri_handler(srv, &g);
    httpd_register_uri_handler(srv, &p);
    ESP_LOGI(TAG, "Webseite /fan angemeldet");
}

static void fan_init(void)
{
    settings_load();

    /* Pin zuerst auf "aus" legen, dann als Ausgang schalten (kein Zucken beim Start). */
    gpio_reset_pin(CONFIG_FAN_GPIO);
    gpio_set_level(CONFIG_FAN_GPIO, !LEVEL_ON);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_FAN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    fan_set(false);

#if !CONFIG_IDF_TARGET_ESP32
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_tsens));
#endif

    ESP_LOGI(TAG, "Luefter: GPIO %d, AN ab %d C, AUS bei %d C, Messung alle %d s",
             CONFIG_FAN_GPIO, s_on_temp, s_off_temp, CONFIG_FAN_CHECK_S);
    xTaskCreate(fan_task, "fan", 3072, NULL, 3, NULL);
}

static int fan_status(char *buf, size_t len)
{
    return snprintf(buf, len,
                    "Chip-Temperatur: %.1f C\nLuefter:         %s (%s)\n",
                    (double)s_temp, s_on ? "AN" : "aus", mode_text(s_mode));
}

MODULE_REGISTER(fan, .name = "fan", .stage = MODULE_STAGE_EARLY, .priority = 10,
                .init = fan_init, .status = fan_status, .http = fan_http,
                .menu_title = "Luefter", .menu_path = "fan", .pins = fan_pins)
