
#include "fan_control.h"

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FAN_RELAY_GPIO GPIO_NUM_4

#define DEFAULT_ON_TEMP 55
#define DEFAULT_OFF_TEMP 48

#define EMERGENCY_TEMP 85.0f
#define RECOVERY_TEMP 70.0f

#define EMERGENCY_WIFI_DELAY (5LL * 60LL * 1000000LL)

#define SENSOR_RETRY_MS 5000
#define CONTROL_INTERVAL_MS 1000

static const char *TAG = "FAN";

static fan_status_t fan_status = {
    .temperature = 0,
    .sensor_ok = false,
    .fan_on = false,
    .emergency_active = false,
    .wifi_suspended = false,
    .mode = FAN_MODE_AUTO,
    .on_temp = DEFAULT_ON_TEMP,
    .off_temp = DEFAULT_OFF_TEMP
};

static portMUX_TYPE fan_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ever_had_valid_reading = false;
static bool emergency_active = false;
static bool wifi_suspended = false;
static bool manual_command_received = false;

static int64_t emergency_started_us = 0;

static void (*thermal_callback)(bool active) = NULL;

static bool task_started = false;


/* GPIO4 HIGH = fan ON, LOW = fan OFF */
static void set_fan_output(bool on)
{
    gpio_set_level(FAN_RELAY_GPIO, on ? 1 : 0);
}


/* Save temperature thresholds */
static void save_settings(void)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open("fan_cfg", NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS konnte nicht geoeffnet werden");
        return;
    }

    nvs_set_i32(handle, "on_temp", fan_status.on_temp);
    nvs_set_i32(handle, "off_temp", fan_status.off_temp);

    err = nvs_commit(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Einstellungen konnten nicht gespeichert werden");
    }

    nvs_close(handle);
}


/* Load temperature thresholds */
static void load_settings(void)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open("fan_cfg", NVS_READONLY, &handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Standardwerte werden verwendet");
        return;
    }

    int32_t on_temp = DEFAULT_ON_TEMP;
    int32_t off_temp = DEFAULT_OFF_TEMP;

    nvs_get_i32(handle, "on_temp", &on_temp);
    nvs_get_i32(handle, "off_temp", &off_temp);

    nvs_close(handle);

    if (off_temp >= 20 &&
        on_temp <= 80 &&
        off_temp < on_temp) {

        fan_status.on_temp = on_temp;
        fan_status.off_temp = off_temp;
    }

    ESP_LOGI(TAG, "Schwellwerte: AN %d C, AUS %d C",
             fan_status.on_temp,
             fan_status.off_temp);
}


/* Register the router's thermal Wi-Fi callback */
void fan_control_set_thermal_callback(void (*callback)(bool active))
{
    portENTER_CRITICAL(&fan_mux);
    thermal_callback = callback;
    bool current_state = wifi_suspended;
    portEXIT_CRITICAL(&fan_mux);

    if (callback != NULL && current_state) {
        callback(true);
    }
}


/* Read current fan status */
void fan_control_get_status(fan_status_t *status)
{
    if (status == NULL) {
        return;
    }

    portENTER_CRITICAL(&fan_mux);
    *status = fan_status;
    status->emergency_active = emergency_active;
    status->wifi_suspended = wifi_suspended;
    portEXIT_CRITICAL(&fan_mux);
}


/* Set AUTO, ON or OFF */
bool fan_control_set_mode(fan_mode_t mode)
{
    if (mode != FAN_MODE_AUTO &&
        mode != FAN_MODE_ON &&
        mode != FAN_MODE_OFF) {
        return false;
    }

    bool fan_on;

    portENTER_CRITICAL(&fan_mux);

    fan_status.mode = mode;
    manual_command_received = true;

    if (emergency_active) {
        fan_status.fan_on = true;
    } else if (mode == FAN_MODE_ON) {
        fan_status.fan_on = true;
    } else if (mode == FAN_MODE_OFF) {
        fan_status.fan_on = false;
    } else if (!ever_had_valid_reading) {
        fan_status.fan_on = false;
    }

    fan_on = fan_status.fan_on;

    portEXIT_CRITICAL(&fan_mux);

    set_fan_output(fan_on);

    ESP_LOGI(TAG, "Lueftermodus: %d", mode);

    return true;
}


/* Change automatic temperature thresholds */
bool fan_control_set_thresholds(int on_temp, int off_temp)
{
    if (off_temp < 20 ||
        on_temp > 80 ||
        off_temp >= on_temp) {
        return false;
    }

    portENTER_CRITICAL(&fan_mux);

    fan_status.on_temp = on_temp;
    fan_status.off_temp = off_temp;

    portEXIT_CRITICAL(&fan_mux);

    save_settings();

    ESP_LOGI(TAG, "Neue Schwellwerte: AN %d C, AUS %d C",
             on_temp, off_temp);

    return true;
}


/* Apply temperature, fan and emergency logic */
static void update_fan_state(bool sensor_ok, float temperature)
{
    bool fan_on;
    bool emergency;
    bool notify_wifi = false;
    bool wifi_state_to_send = false;

    void (*callback)(bool active) = NULL;

    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&fan_mux);

    if (sensor_ok) {
        fan_status.temperature = temperature;
        fan_status.sensor_ok = true;
        ever_had_valid_reading = true;
    } else {
        fan_status.sensor_ok = false;
    }

    /* Enter thermal emergency */
    if (sensor_ok && temperature >= EMERGENCY_TEMP) {
        if (!emergency_active) {
            emergency_active = true;
            emergency_started_us = now;

            ESP_LOGE(TAG,
                     "THERMISCHER NOTFALL: %.1f C",
                     temperature);
        }
    }

    /* Release only after 5 minutes and valid temperature below 70 C */
    if (emergency_active &&
        sensor_ok &&
        temperature < RECOVERY_TEMP &&
        now - emergency_started_us >= EMERGENCY_WIFI_DELAY) {

        emergency_active = false;

        ESP_LOGW(TAG, "Thermischer Notfall beendet");
    }

    bool new_wifi_state = emergency_active;

    if (new_wifi_state != wifi_suspended) {
        wifi_suspended = new_wifi_state;
        notify_wifi = true;
        wifi_state_to_send = new_wifi_state;
        callback = thermal_callback;
    }

    /* Fan control */
    if (emergency_active) {
        fan_status.fan_on = true;
    }
    else if (fan_status.mode == FAN_MODE_ON &&
             manual_command_received) {
        fan_status.fan_on = true;
    }
    else if (fan_status.mode == FAN_MODE_OFF &&
             manual_command_received) {
        fan_status.fan_on = false;
    }
    else if (!sensor_ok && ever_had_valid_reading &&
             fan_status.mode == FAN_MODE_AUTO) {
        /* Fail-safe after a previously valid measurement */
        fan_status.fan_on = true;
    }
    else if (sensor_ok &&
             fan_status.mode == FAN_MODE_AUTO) {

        if (!fan_status.fan_on &&
            temperature >= fan_status.on_temp) {
            fan_status.fan_on = true;
        }

        if (fan_status.fan_on &&
            temperature <= fan_status.off_temp) {
            fan_status.fan_on = false;
        }
    }

    fan_status.emergency_active = emergency_active;
    fan_status.wifi_suspended = wifi_suspended;

    fan_on = fan_status.fan_on;
    emergency = emergency_active;

    float logged_temperature = fan_status.temperature;

    portEXIT_CRITICAL(&fan_mux);

    /* Apply GPIO outside critical section */
    set_fan_output(fan_on);

    /* Notify router outside critical section */
    if (notify_wifi && callback != NULL) {
        callback(wifi_state_to_send);
    }

    ESP_LOGI(TAG,
             "Temperatur: %.1f C | Sensor: %s | Luefter: %s | Notfall: %s",
             logged_temperature,
             sensor_ok ? "OK" : "FEHLER",
             fan_on ? "AN" : "AUS",
             emergency ? "JA" : "NEIN");
}


/* Temperature sensor task with automatic retries */
static void fan_control_task(void *arg)
{
    temperature_sensor_handle_t temp_handle = NULL;

    temperature_sensor_config_t temp_config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);

    while (1) {

        /* Install sensor */
        esp_err_t err =
            temperature_sensor_install(&temp_config, &temp_handle);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Sensorinstallation fehlgeschlagen: %s",
                     esp_err_to_name(err));

            update_fan_state(false, 0);

            vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
            continue;
        }

        /* Enable sensor */
        err = temperature_sensor_enable(temp_handle);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Sensoraktivierung fehlgeschlagen: %s",
                     esp_err_to_name(err));

            temperature_sensor_uninstall(temp_handle);
            temp_handle = NULL;

            update_fan_state(false, 0);

            vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
            continue;
        }

        ESP_LOGI(TAG, "Temperatursensor aktiv");

        bool retry_sensor = false;

        while (!retry_sensor) {

            float temperature = 0;

            err = temperature_sensor_get_celsius(
                temp_handle, &temperature);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Temperaturmessung fehlgeschlagen: %s",
                         esp_err_to_name(err));

                retry_sensor = true;
                break;
            }

            update_fan_state(true, temperature);

            vTaskDelay(pdMS_TO_TICKS(CONTROL_INTERVAL_MS));
        }

        temperature_sensor_disable(temp_handle);
        temperature_sensor_uninstall(temp_handle);
        temp_handle = NULL;

        update_fan_state(false, 0);

        ESP_LOGW(TAG, "Sensor wird in 5 Sekunden erneut versucht");

        vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
    }
}


/* Initialize fan control */
void fan_control_start(void)
{
    if (task_started) {
        return;
    }

    task_started = true;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FAN_RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* Fan must remain OFF until the first valid measurement */
    set_fan_output(false);

    load_settings();

    BaseType_t result = xTaskCreate(
        fan_control_task,
        "fan_control",
        4096,
        NULL,
        5,
        NULL
    );

    if (result != pdPASS) {
        task_started = false;
        ESP_LOGE(TAG, "Fan-Control-Task konnte nicht gestartet werden");
    }
}
