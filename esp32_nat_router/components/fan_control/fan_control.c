#include "fan_control.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FAN_RELAY_GPIO GPIO_NUM_4

#define DEFAULT_ON_TEMP  55
#define DEFAULT_OFF_TEMP 48

static const char *TAG = "FAN";

static fan_status_t fan_status = {
    .temperature = 0,
    .sensor_ok = false,
    .fan_on = false,
    .mode = FAN_MODE_AUTO,
    .on_temp = DEFAULT_ON_TEMP,
    .off_temp = DEFAULT_OFF_TEMP
};

static portMUX_TYPE fan_mux = portMUX_INITIALIZER_UNLOCKED;

static void load_settings(void)
{
    nvs_handle_t handle;

    if (nvs_open("fan_cfg", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS konnte nicht geöffnet werden");
        return;
    }

    int32_t value;

    if (nvs_get_i32(handle, "on_temp", &value) == ESP_OK &&
        value >= 20 && value <= 80) {
        fan_status.on_temp = value;
    }

    if (nvs_get_i32(handle, "off_temp", &value) == ESP_OK &&
        value >= 20 && value < fan_status.on_temp) {
        fan_status.off_temp = value;
    }

    if (nvs_get_i32(handle, "mode", &value) == ESP_OK &&
        value >= FAN_MODE_AUTO && value <= FAN_MODE_OFF) {
        fan_status.mode = (fan_mode_t)value;
    }

    nvs_close(handle);
}

void fan_control_get_status(fan_status_t *status)
{
    if (status == NULL) {
        return;
    }

    portENTER_CRITICAL(&fan_mux);
    *status = fan_status;
    portEXIT_CRITICAL(&fan_mux);
}

bool fan_control_set_mode(fan_mode_t mode)
{
    if (mode < FAN_MODE_AUTO || mode > FAN_MODE_OFF) {
        return false;
    }

    nvs_handle_t handle;

    if (nvs_open("fan_cfg", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_i32(handle, "mode", mode);

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    portENTER_CRITICAL(&fan_mux);
    fan_status.mode = mode;
    portEXIT_CRITICAL(&fan_mux);

    return true;
}

bool fan_control_set_thresholds(int on_temp, int off_temp)
{
    if (on_temp < 20 || on_temp > 80 ||
        off_temp < 20 || off_temp >= on_temp) {
        return false;
    }

    nvs_handle_t handle;

    if (nvs_open("fan_cfg", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_i32(handle, "on_temp", on_temp);

    if (err == ESP_OK) {
        err = nvs_set_i32(handle, "off_temp", off_temp);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    portENTER_CRITICAL(&fan_mux);
    fan_status.on_temp = on_temp;
    fan_status.off_temp = off_temp;
    portEXIT_CRITICAL(&fan_mux);

    return true;
}

static void fan_control_task(void *pvParameters)
{
    temperature_sensor_handle_t temp_handle = NULL;

    temperature_sensor_config_t temp_config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);

    esp_err_t err = temperature_sensor_install(
        &temp_config, &temp_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Temperatursensor konnte nicht installiert werden");
        vTaskDelete(NULL);
        return;
    }

    err = temperature_sensor_enable(temp_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Temperatursensor konnte nicht aktiviert werden");
        vTaskDelete(NULL);
        return;
    }

    gpio_reset_pin(FAN_RELAY_GPIO);
    gpio_set_direction(FAN_RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(FAN_RELAY_GPIO, 1);

    while (1) {
        float temperature = 0;
        bool sensor_ok =
            temperature_sensor_get_celsius(temp_handle, &temperature) == ESP_OK;

        portENTER_CRITICAL(&fan_mux);

        if (sensor_ok) {
            fan_status.temperature = temperature;
            fan_status.sensor_ok = true;
        } else {
            fan_status.sensor_ok = false;
        }

        if (fan_status.mode == FAN_MODE_ON) {
            fan_status.fan_on = true;
        } else if (fan_status.mode == FAN_MODE_OFF) {
            fan_status.fan_on = false;
        } else if (sensor_ok) {
            if (!fan_status.fan_on &&
                temperature >= fan_status.on_temp) {
                fan_status.fan_on = true;
            }

            if (fan_status.fan_on &&
                temperature <= fan_status.off_temp) {
                fan_status.fan_on = false;
            }
        }

        bool fan_on = fan_status.fan_on;
        float current_temp = fan_status.temperature;

        portEXIT_CRITICAL(&fan_mux);

        gpio_set_level(FAN_RELAY_GPIO, fan_on ? 0 : 1);

        ESP_LOGI(TAG, "Temperatur: %.1f C | Luefter: %s",
                 current_temp, fan_on ? "AN" : "AUS");

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void fan_control_start(void)
{
    load_settings();

    gpio_reset_pin(FAN_RELAY_GPIO);
    gpio_set_direction(FAN_RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(FAN_RELAY_GPIO, 1);

    xTaskCreate(
        fan_control_task,
        "fan_control",
        4096,
        NULL,
        5,
        NULL
    );
}
