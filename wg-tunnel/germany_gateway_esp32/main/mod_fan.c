/*
 * Temperaturgesteuerter Luefter ueber ein Relais (oder Transistor).
 *
 * - alle FAN_CHECK_S Sekunden wird die Chip-Temperatur gemessen
 * - ab FAN_ON_TEMP_C schaltet der Luefter ein, bei FAN_OFF_TEMP_C wieder aus
 *   (Hysterese, damit das Relais nicht dauernd klickt)
 * - zwischen zwei Schaltvorgaengen liegen mindestens FAN_MIN_HOLD_S Sekunden
 * - schlaegt die Messung mehrfach fehl, laeuft der Luefter sicherheitshalber
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "module_api.h"

#if CONFIG_IDF_TARGET_ESP32
/* Der klassische ESP32 hat keinen offiziell unterstuetzten Sensor. Diese alte
 * interne Funktion liefert oft nur den festen Wert 128 (= "kein Wert"). In dem
 * Fall laeuft der Luefter dauerhaft (sichere Rueckfallloesung, siehe fan_task). */
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

static volatile float s_temp = -100.0f;
static volatile bool s_on = false;

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
        float t;
        if (read_temp(&t)) {
            fails = 0;
            s_temp = t;
            TickType_t now = xTaskGetTickCount();
            bool hold_ok = (now - last_switch) >= pdMS_TO_TICKS(CONFIG_FAN_MIN_HOLD_S * 1000);

            if (!s_on && t >= CONFIG_FAN_ON_TEMP_C && hold_ok) {
                fan_set(true);
                last_switch = now;
                ESP_LOGW(TAG, "%.1f C >= %d C: Luefter AN", t, CONFIG_FAN_ON_TEMP_C);
            } else if (s_on && t <= CONFIG_FAN_OFF_TEMP_C && hold_ok) {
                fan_set(false);
                last_switch = now;
                ESP_LOGI(TAG, "%.1f C <= %d C: Luefter AUS", t, CONFIG_FAN_OFF_TEMP_C);
            }
            if (++log_counter >= 6) { /* alle ~30 s eine Zeile */
                log_counter = 0;
                ESP_LOGI(TAG, "Chip %.1f C, Luefter %s", t, s_on ? "AN" : "aus");
            }
        } else if (++fails >= 3) {
            if (!s_on) {
                ESP_LOGE(TAG, "Temperaturmessung faellt aus, Luefter sicherheitshalber AN");
                fan_set(true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FAN_CHECK_S * 1000));
    }
}

static void fan_init(void)
{
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
             CONFIG_FAN_GPIO, CONFIG_FAN_ON_TEMP_C, CONFIG_FAN_OFF_TEMP_C, CONFIG_FAN_CHECK_S);
    xTaskCreate(fan_task, "fan", 3072, NULL, 3, NULL);
}

static int fan_status(char *buf, size_t len)
{
    return snprintf(buf, len, "Chip-Temperatur: %.1f C\nLuefter:         %s\n",
                    (double)s_temp, s_on ? "AN" : "aus");
}

MODULE_REGISTER(fan, .name = "fan", .stage = MODULE_STAGE_EARLY, .priority = 10,
                .init = fan_init, .status = fan_status)
