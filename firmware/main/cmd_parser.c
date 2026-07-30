#include "cmd_parser.h"
#include "board_config.h"
#include "power_ctl.h"
#include "usb_bridge.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cmd_parser";

typedef void (*cmd_handler_t)(int argc, char **argv, char *response, size_t response_size);

typedef struct {
    const char *name;
    cmd_handler_t handler;
} cmd_entry_t;

static bool parse_uint(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0' || s[0] == '-') {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = (uint32_t) v;
    return true;
}

static void handle_ping(int argc, char **argv, char *response, size_t response_size)
{
    (void) argc;
    (void) argv;
    snprintf(response, response_size, "OK PONG");
}

static void handle_version(int argc, char **argv, char *response, size_t response_size)
{
    (void) argc;
    (void) argv;
    snprintf(response, response_size, "OK glasnost-jig %s idf %s", GLASNOST_FW_VERSION, esp_get_idf_version());
}

static void handle_power(int argc, char **argv, char *response, size_t response_size)
{
    if (argc < 2) {
        snprintf(response, response_size, "ERR BAD_ARG");
        return;
    }

    if (strcasecmp(argv[1], "ON") == 0) {
        esp_err_t err = power_ctl_set(true);
        snprintf(response, response_size, err == ESP_ERR_NOT_SUPPORTED ? "ERR NOT_WIRED" : "OK POWER ON");
    } else if (strcasecmp(argv[1], "OFF") == 0) {
        esp_err_t err = power_ctl_set(false);
        snprintf(response, response_size, err == ESP_ERR_NOT_SUPPORTED ? "ERR NOT_WIRED" : "OK POWER OFF");
    } else if (strcasecmp(argv[1], "CYCLE") == 0) {
        uint32_t ms = BOARD_POWER_CYCLE_DEFAULT_MS;
        if (argc >= 3 && (!parse_uint(argv[2], &ms) || ms > BOARD_POWER_CYCLE_MAX_MS)) {
            snprintf(response, response_size, "ERR BAD_ARG");
            return;
        }
        esp_err_t err = power_ctl_cycle(ms);
        snprintf(response, response_size, err == ESP_ERR_NOT_SUPPORTED ? "ERR NOT_WIRED" : "OK POWER CYCLE");
    } else if (strcasecmp(argv[1], "STATUS") == 0) {
        snprintf(response, response_size, "OK POWER %s", power_ctl_get_state() == POWER_STATE_ON ? "ON" : "OFF");
    } else {
        snprintf(response, response_size, "ERR BAD_ARG");
    }
}

static void handle_reset(int argc, char **argv, char *response, size_t response_size)
{
    uint32_t ms = BOARD_RESET_PULSE_DEFAULT_MS;
    if (argc >= 2 && (!parse_uint(argv[1], &ms) || ms > BOARD_RESET_PULSE_MAX_MS)) {
        snprintf(response, response_size, "ERR BAD_ARG");
        return;
    }
    esp_err_t err = power_ctl_reset_pulse(ms);
    snprintf(response, response_size, err == ESP_ERR_NOT_SUPPORTED ? "ERR NOT_WIRED" : "OK RESET");
}

static void handle_uart(int argc, char **argv, char *response, size_t response_size)
{
    if (argc < 2) {
        snprintf(response, response_size, "ERR BAD_ARG");
        return;
    }

    if (strcasecmp(argv[1], "BAUD") == 0) {
        uint32_t baud;
        if (argc < 3 || !parse_uint(argv[2], &baud) || baud == 0) {
            snprintf(response, response_size, "ERR BAD_ARG");
            return;
        }
        esp_err_t err = usb_bridge_set_dut_baud(baud);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            snprintf(response, response_size, "ERR NOT_WIRED");
        } else if (err != ESP_OK) {
            snprintf(response, response_size, "ERR BAD_ARG");
        } else {
            snprintf(response, response_size, "OK UART BAUD %" PRIu32, baud);
        }
    } else if (strcasecmp(argv[1], "STATUS") == 0) {
        if (!usb_bridge_dut_uart_wired()) {
            snprintf(response, response_size, "ERR NOT_WIRED");
        } else {
            snprintf(response, response_size, "OK UART BAUD %" PRIu32, usb_bridge_get_dut_baud());
        }
    } else {
        snprintf(response, response_size, "ERR BAD_ARG");
    }
}

static void handle_status(int argc, char **argv, char *response, size_t response_size)
{
    (void) argc;
    (void) argv;
    uint32_t baud = usb_bridge_dut_uart_wired() ? usb_bridge_get_dut_baud() : 0;
    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    snprintf(response, response_size, "OK POWER=%s UART_BAUD=%" PRIu32 " UPTIME=%lld",
             power_ctl_get_state() == POWER_STATE_ON ? "ON" : "OFF", baud, (long long) uptime_sec);
}

static void handle_not_implemented(int argc, char **argv, char *response, size_t response_size)
{
    (void) argc;
    (void) argv;
    snprintf(response, response_size, "ERR NOT_IMPLEMENTED");
}

static const cmd_entry_t k_commands[] = {
    { "PING", handle_ping },
    { "VERSION", handle_version },
    { "POWER", handle_power },
    { "RESET", handle_reset },
    { "UART", handle_uart },
    { "STATUS", handle_status },
    { "SPI", handle_not_implemented },
    { "I2C", handle_not_implemented },
};

void cmd_parser_init(void)
{
    ESP_LOGI(TAG, "command table ready, %d verbs", (int) (sizeof(k_commands) / sizeof(k_commands[0])));
}

void cmd_parser_handle_line(const char *line, char *response, size_t response_size)
{
    char work_buf[CMD_LINE_MAX_LEN + 1];
    strncpy(work_buf, line, sizeof(work_buf) - 1);
    work_buf[sizeof(work_buf) - 1] = '\0';

    char *argv[CMD_MAX_TOKENS];
    int argc = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(work_buf, " \t", &saveptr);
    while (tok != NULL && argc < CMD_MAX_TOKENS) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    if (argc == 0) {
        snprintf(response, response_size, "ERR UNKNOWN_COMMAND");
        return;
    }

    for (size_t i = 0; i < sizeof(k_commands) / sizeof(k_commands[0]); i++) {
        if (strcasecmp(argv[0], k_commands[i].name) == 0) {
            k_commands[i].handler(argc, argv, response, response_size);
            return;
        }
    }
    snprintf(response, response_size, "ERR UNKNOWN_COMMAND");
}
