#include "cmd_channel.h"
#include "power_ctl.h"
#include "usb_bridge.h"

#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    power_ctl_init();
    ESP_ERROR_CHECK(usb_bridge_init());
    ESP_ERROR_CHECK(cmd_channel_init());
    ESP_LOGI(TAG, "glasnost jig ready");
}
