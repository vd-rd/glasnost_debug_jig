#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t usb_bridge_init(void);

esp_err_t usb_bridge_set_dut_baud(uint32_t baud);
uint32_t usb_bridge_get_dut_baud(void);
bool usb_bridge_dut_uart_wired(void);
