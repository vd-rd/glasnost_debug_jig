#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    POWER_STATE_OFF = 0,
    POWER_STATE_ON = 1,
} power_state_t;

void power_ctl_init(void);

esp_err_t power_ctl_set(bool on);
power_state_t power_ctl_get_state(void);
esp_err_t power_ctl_cycle(uint32_t off_ms);
esp_err_t power_ctl_reset_pulse(uint32_t pulse_ms);
