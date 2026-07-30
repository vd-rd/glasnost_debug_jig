#include "power_ctl.h"
#include "board_config.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "power_ctl";

static power_state_t s_power_state = POWER_STATE_OFF;
static esp_timer_handle_t s_power_cycle_timer;
static esp_timer_handle_t s_reset_timer;

static inline bool power_switch_wired(void)
{
    return BOARD_POWER_SWITCH_GPIO != BOARD_GPIO_UNWIRED;
}

static inline bool reset_wired(void)
{
    return BOARD_RESET_GPIO != BOARD_GPIO_UNWIRED;
}

static void power_switch_write(bool on)
{
    gpio_set_level(BOARD_POWER_SWITCH_GPIO, on ? BOARD_POWER_SWITCH_ACTIVE_LEVEL : !BOARD_POWER_SWITCH_ACTIVE_LEVEL);
}

/*
 * gpio is a runtime parameter (never the BOARD_GPIO_UNWIRED literal at the
 * call sites below) rather than the board_config.h macro inlined directly,
 * so `1ULL << gpio` doesn't get compile-time-folded against a -1 sentinel
 * and flagged as a negative shift count.
 *
 * The idle level is written before gpio_config() switches the pin to
 * output mode, not after: gpio_set_level() only touches the GPIO output
 * data register regardless of current pin direction, so this avoids a
 * transient glitch on the opposite level between enabling the output
 * driver and setting its intended idle state (matters once a future board
 * wires an idle-high, active-low signal here).
 */
static void configure_output_pin(int gpio, int idle_level)
{
    gpio_set_level(gpio, idle_level);
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

static void power_cycle_timer_cb(void *arg)
{
    (void) arg;
    power_switch_write(true);
    s_power_state = POWER_STATE_ON;
    ESP_LOGI(TAG, "power cycle complete, power ON");
}

static void reset_timer_cb(void *arg)
{
    (void) arg;
    gpio_set_level(BOARD_RESET_GPIO, !BOARD_RESET_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "reset pulse complete");
}

void power_ctl_init(void)
{
    if (power_switch_wired()) {
        configure_output_pin(BOARD_POWER_SWITCH_GPIO, !BOARD_POWER_SWITCH_ACTIVE_LEVEL);
    } else {
        ESP_LOGW(TAG, "power switch pin not wired, POWER commands will return NOT_WIRED");
    }
    s_power_state = POWER_STATE_OFF;

    if (reset_wired()) {
        configure_output_pin(BOARD_RESET_GPIO, !BOARD_RESET_ACTIVE_LEVEL);
    } else {
        ESP_LOGW(TAG, "reset pin not wired, RESET command will return NOT_WIRED");
    }

    const esp_timer_create_args_t power_cycle_timer_args = {
        .callback = &power_cycle_timer_cb,
        .name = "power_cycle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&power_cycle_timer_args, &s_power_cycle_timer));

    const esp_timer_create_args_t reset_timer_args = {
        .callback = &reset_timer_cb,
        .name = "reset_pulse",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reset_timer_args, &s_reset_timer));
}

esp_err_t power_ctl_set(bool on)
{
    if (!power_switch_wired()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_timer_stop(s_power_cycle_timer);
    power_switch_write(on);
    s_power_state = on ? POWER_STATE_ON : POWER_STATE_OFF;
    ESP_LOGI(TAG, "power set to %s", on ? "ON" : "OFF");
    return ESP_OK;
}

power_state_t power_ctl_get_state(void)
{
    return s_power_state;
}

/*
 * The off->on transition is completed by power_cycle_timer_cb on the
 * esp_timer service task, not blocked on here: cmd_task (the caller, via
 * cmd_parser) must stay free to keep answering other control-channel
 * commands (POWER STATUS, PING, ...) while a cycle is in flight, and never
 * block for a caller-chosen duration itself.
 */
esp_err_t power_ctl_cycle(uint32_t off_ms)
{
    if (!power_switch_wired()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_timer_stop(s_power_cycle_timer);
    power_switch_write(false);
    s_power_state = POWER_STATE_OFF;
    esp_err_t err = esp_timer_start_once(s_power_cycle_timer, (uint64_t) off_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start power-cycle timer: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "power cycle started, %" PRIu32 " ms off", off_ms);
    return ESP_OK;
}

esp_err_t power_ctl_reset_pulse(uint32_t pulse_ms)
{
    if (!reset_wired()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_timer_stop(s_reset_timer);
    gpio_set_level(BOARD_RESET_GPIO, BOARD_RESET_ACTIVE_LEVEL);
    esp_err_t err = esp_timer_start_once(s_reset_timer, (uint64_t) pulse_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start reset-pulse timer: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "reset pulse started, %" PRIu32 " ms", pulse_ms);
    return ESP_OK;
}
