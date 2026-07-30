#include "usb_bridge.h"
#include "board_config.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "usb_bridge";

static uint32_t s_dut_baud = BOARD_DUT_UART_BAUD_DEFAULT;
static bool s_dut_uart_wired = false;
static StreamBufferHandle_t s_dut_tx_stream;

static inline bool dut_uart_pins_wired(void)
{
    return BOARD_DUT_UART_TX_GPIO != BOARD_GPIO_UNWIRED && BOARD_DUT_UART_RX_GPIO != BOARD_GPIO_UNWIRED;
}

/*
 * tinyusb_cdcacm_write_queue() only stages what fits in the 512-byte TX
 * buffer and silently truncates the rest, so a short write here would mean
 * silently dropped DUT console output whenever the host isn't reading
 * promptly. Retry until fully queued, bounded by a timeout so a host that
 * genuinely stopped reading can't wedge the caller forever.
 */
static void cdc_write_reliable(tinyusb_cdcacm_itf_t itf, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_BRIDGE_TX_DRAIN_TIMEOUT_MS);
    while (sent < len) {
        sent += tinyusb_cdcacm_write_queue(itf, data + sent, len - sent);
        if (sent >= len) {
            break;
        }
        tinyusb_cdcacm_write_flush(itf, 0);
        if ((int32_t) (xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "TX backpressure on itf %d, dropping %u bytes", (int) itf, (unsigned) (len - sent));
            break;
        }
        vTaskDelay(1);
    }
    tinyusb_cdcacm_write_flush(itf, 0);
}

/*
 * Runs on esp_tinyusb's single shared USB-service task (also serving
 * CDC_ACM_1). uart_write_bytes() can block for as long as the DUT UART's TX
 * ring buffer stays full, so it must never be called from here directly —
 * that would stall the whole USB device. Hand bytes off to dut_uart_tx_task
 * over a stream buffer instead; the non-blocking send here just drops on
 * sustained backpressure rather than ever blocking this callback.
 */
static void cdc0_rx_cb(int itf, cdcacm_event_t *event)
{
    (void) itf;
    (void) event;
    if (!s_dut_uart_wired) {
        return;
    }
    uint8_t buf[USB_BRIDGE_RX_STAGING_SIZE];
    size_t rx_size = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &rx_size) != ESP_OK || rx_size == 0) {
        return;
    }
    size_t queued = xStreamBufferSend(s_dut_tx_stream, buf, rx_size, 0);
    if (queued < rx_size) {
        ESP_LOGW(TAG, "DUT UART TX backpressure, dropping %u bytes", (unsigned) (rx_size - queued));
    }
}

static void cdc0_line_coding_cb(int itf, cdcacm_event_t *event)
{
    (void) itf;
    if (!s_dut_uart_wired) {
        return;
    }
    uint32_t baud = event->line_coding_changed_data.p_line_coding->bit_rate;
    if (baud == 0) {
        return;
    }
    if (uart_set_baudrate(BOARD_DUT_UART_PORT, baud) == ESP_OK) {
        s_dut_baud = baud;
        ESP_LOGI(TAG, "DUT UART baud set to %" PRIu32 " via host line coding", baud);
    }
}

static void dut_uart_rx_task(void *arg)
{
    (void) arg;
    uint8_t buf[USB_BRIDGE_RX_STAGING_SIZE];
    for (;;) {
        int len = uart_read_bytes(BOARD_DUT_UART_PORT, buf, sizeof(buf),
                                   pdMS_TO_TICKS(USB_BRIDGE_UART_READ_TIMEOUT_MS));
        if (len > 0) {
            cdc_write_reliable(TINYUSB_CDC_ACM_0, buf, (size_t) len);
        }
    }
}

static void dut_uart_tx_task(void *arg)
{
    (void) arg;
    uint8_t buf[USB_BRIDGE_RX_STAGING_SIZE];
    for (;;) {
        size_t len = xStreamBufferReceive(s_dut_tx_stream, buf, sizeof(buf), portMAX_DELAY);
        if (len > 0) {
            uart_write_bytes(BOARD_DUT_UART_PORT, (const char *) buf, len);
        }
    }
}

esp_err_t usb_bridge_init(void)
{
    s_dut_uart_wired = dut_uart_pins_wired();
    if (s_dut_uart_wired) {
        uart_config_t uart_cfg = {
            .baud_rate = BOARD_DUT_UART_BAUD_DEFAULT,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_param_config(BOARD_DUT_UART_PORT, &uart_cfg));
        ESP_ERROR_CHECK(uart_set_pin(BOARD_DUT_UART_PORT, BOARD_DUT_UART_TX_GPIO, BOARD_DUT_UART_RX_GPIO,
                                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(BOARD_DUT_UART_PORT, BOARD_DUT_UART_RX_BUF_SIZE,
                                             BOARD_DUT_UART_TX_BUF_SIZE, 0, NULL, 0));
        s_dut_baud = BOARD_DUT_UART_BAUD_DEFAULT;

        s_dut_tx_stream = xStreamBufferCreate(USB_BRIDGE_DUT_TX_STREAM_SIZE, 1);
        if (s_dut_tx_stream == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGW(TAG, "DUT UART pins not wired, passthrough disabled");
    }

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm0_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &cdc0_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = &cdc0_line_coding_cb,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm0_cfg));

    const tinyusb_config_cdcacm_t acm1_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_1,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm1_cfg));

    if (s_dut_uart_wired) {
        if (xTaskCreate(dut_uart_rx_task, "dut_uart_rx", USB_BRIDGE_UART_TASK_STACK_SIZE, NULL,
                         USB_BRIDGE_UART_TASK_PRIORITY, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create dut_uart_rx task");
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(dut_uart_tx_task, "dut_uart_tx", USB_BRIDGE_DUT_TX_TASK_STACK_SIZE, NULL,
                         USB_BRIDGE_DUT_TX_TASK_PRIORITY, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create dut_uart_tx task");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t usb_bridge_set_dut_baud(uint32_t baud)
{
    if (!s_dut_uart_wired) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (baud == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = uart_set_baudrate(BOARD_DUT_UART_PORT, baud);
    if (err == ESP_OK) {
        s_dut_baud = baud;
    }
    return err;
}

uint32_t usb_bridge_get_dut_baud(void)
{
    return s_dut_baud;
}

bool usb_bridge_dut_uart_wired(void)
{
    return s_dut_uart_wired;
}
