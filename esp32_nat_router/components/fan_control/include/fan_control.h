
#pragma once

#include <stdbool.h>

typedef enum {
    FAN_MODE_AUTO = 0,
    FAN_MODE_ON,
    FAN_MODE_OFF
} fan_mode_t;

typedef struct {
    float temperature;
    bool sensor_ok;
    bool fan_on;
    bool emergency_active;
    bool wifi_suspended;
    fan_mode_t mode;
    int on_temp;
    int off_temp;
} fan_status_t;

void fan_control_start(void);
void fan_control_get_status(fan_status_t *status);

bool fan_control_set_mode(fan_mode_t mode);
bool fan_control_set_thresholds(int on_temp, int off_temp);

void fan_control_set_thermal_callback(void (*callback)(bool active));
