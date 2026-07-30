#pragma once

#include "driver/uart.h"

#define GLASNOST_FW_VERSION "0.1.0"

#define BOARD_GPIO_UNWIRED (-1)

/*
 * Placeholder pin assignments. Hardware (schematic/PCB) is being designed
 * separately from this firmware; the values below are safe generic GPIOs
 * on an ESP32-S3 (not strapping pins GPIO0/3/45/46, not the native-USB
 * D+/D- pins GPIO19/20, not the default console UART pins GPIO43/44) and
 * will be replaced once the real board revision is known. This file is the
 * only place pin numbers appear.
 */
#define BOARD_DUT_UART_TX_GPIO   17
#define BOARD_DUT_UART_RX_GPIO   18
#define BOARD_POWER_SWITCH_GPIO  4
#define BOARD_RESET_GPIO         BOARD_GPIO_UNWIRED

#define BOARD_POWER_SWITCH_ACTIVE_LEVEL 1
#define BOARD_RESET_ACTIVE_LEVEL        0

#define BOARD_DUT_UART_PORT         UART_NUM_1
#define BOARD_DUT_UART_BAUD_DEFAULT 115200
#define BOARD_DUT_UART_RX_BUF_SIZE  2048
#define BOARD_DUT_UART_TX_BUF_SIZE  2048

#define BOARD_POWER_CYCLE_DEFAULT_MS 1000
#define BOARD_RESET_PULSE_DEFAULT_MS 200
#define BOARD_POWER_CYCLE_MAX_MS     60000
#define BOARD_RESET_PULSE_MAX_MS     60000

#define CMD_LINE_MAX_LEN  128
#define CMD_MAX_TOKENS    4

#define USB_BRIDGE_RX_STAGING_SIZE       256
#define USB_BRIDGE_UART_TASK_STACK_SIZE  4096
#define USB_BRIDGE_UART_TASK_PRIORITY    5
#define USB_BRIDGE_UART_READ_TIMEOUT_MS  20

#define USB_BRIDGE_DUT_TX_STREAM_SIZE      2048
#define USB_BRIDGE_DUT_TX_TASK_STACK_SIZE  4096
#define USB_BRIDGE_DUT_TX_TASK_PRIORITY    5
#define USB_BRIDGE_TX_DRAIN_TIMEOUT_MS     200

#define CMD_CHANNEL_RX_STAGING_SIZE 64
#define CMD_RESPONSE_MAX_LEN        160
#define CMD_QUEUE_LEN               4
#define CMD_TASK_STACK_SIZE         4096
#define CMD_TASK_PRIORITY           5
#define CMD_TX_DRAIN_TIMEOUT_MS     200
