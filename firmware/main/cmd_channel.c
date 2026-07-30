#include "cmd_channel.h"
#include "board_config.h"
#include "cmd_parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "cmd_channel";

/*
 * cmd_channel_rx_cb runs on esp_tinyusb's single shared USB task, which also
 * services CDC_ACM_0 (DUT UART passthrough). Command handlers can block for
 * caller-chosen durations (POWER CYCLE/RESET pulse widths), so the callback
 * must never run them inline — that would stall the whole USB device, not
 * just the control channel. It only assembles lines and hands complete ones
 * to cmd_task over a queue; cmd_task does the actual blocking work.
 */
typedef struct {
    bool overflow;
    char line[CMD_LINE_MAX_LEN + 1];
} cmd_line_msg_t;

static char s_line_buf[CMD_LINE_MAX_LEN + 1];
static size_t s_line_len = 0;
static bool s_line_overflow = false;
static QueueHandle_t s_cmd_queue;

/*
 * tinyusb_cdcacm_write_queue() only stages what fits in the 512-byte TX
 * buffer and silently truncates the rest. Retry until the whole response is
 * queued, bounded by a timeout so a host that stopped reading can't wedge
 * cmd_task forever (this runs in cmd_task, never in the RX callback, so
 * blocking here is fine).
 */
static void cdc_write_reliable(tinyusb_cdcacm_itf_t itf, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CMD_TX_DRAIN_TIMEOUT_MS);
    while (sent < len) {
        sent += tinyusb_cdcacm_write_queue(itf, data + sent, len - sent);
        if (sent >= len) {
            break;
        }
        tinyusb_cdcacm_write_flush(itf, 0);
        if ((int32_t) (xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "response TX backpressure, dropping %u bytes", (unsigned) (len - sent));
            break;
        }
        vTaskDelay(1);
    }
    tinyusb_cdcacm_write_flush(itf, 0);
}

static void send_response(const char *resp)
{
    char line[CMD_RESPONSE_MAX_LEN + 1];
    int written = snprintf(line, sizeof(line), "%s\n", resp);
    size_t len = (written < 0) ? 0 : ((size_t) written >= sizeof(line) ? sizeof(line) - 1 : (size_t) written);
    cdc_write_reliable(TINYUSB_CDC_ACM_1, (const uint8_t *) line, len);
}

static void reset_line_state(void)
{
    s_line_len = 0;
    s_line_overflow = false;
}

static void enqueue_line(void)
{
    cmd_line_msg_t msg = { .overflow = s_line_overflow };
    if (!msg.overflow) {
        memcpy(msg.line, s_line_buf, s_line_len);
        msg.line[s_line_len] = '\0';
        if (s_line_len > 0 && msg.line[s_line_len - 1] == '\r') {
            msg.line[s_line_len - 1] = '\0';
        }
    }
    if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropping line");
    }
}

static void cmd_channel_rx_cb(int itf, cdcacm_event_t *event)
{
    (void) itf;
    (void) event;
    uint8_t buf[CMD_CHANNEL_RX_STAGING_SIZE];
    size_t rx_size = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_1, buf, sizeof(buf), &rx_size) != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < rx_size; i++) {
        char c = (char) buf[i];
        if (c == '\n') {
            enqueue_line();
            reset_line_state();
            continue;
        }
        if (s_line_len >= CMD_LINE_MAX_LEN) {
            s_line_overflow = true;
            continue;
        }
        s_line_buf[s_line_len++] = c;
    }
}

static void cmd_task(void *arg)
{
    (void) arg;
    cmd_line_msg_t msg;
    char response[CMD_RESPONSE_MAX_LEN];
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.overflow) {
            send_response("ERR LINE_TOO_LONG");
            continue;
        }
        cmd_parser_handle_line(msg.line, response, sizeof(response));
        send_response(response);
    }
}

esp_err_t cmd_channel_init(void)
{
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_line_msg_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(cmd_task, "cmd_task", CMD_TASK_STACK_SIZE, NULL, CMD_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_1, CDC_EVENT_RX, &cmd_channel_rx_cb);
    if (err != ESP_OK) {
        return err;
    }
    cmd_parser_init();
    ESP_LOGI(TAG, "control channel ready on CDC_ACM_1");
    return ESP_OK;
}
