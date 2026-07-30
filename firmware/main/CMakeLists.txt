idf_component_register(
    SRCS
        "main.c"
        "usb_bridge.c"
        "cmd_channel.c"
        "cmd_parser.c"
        "power_ctl.c"
    INCLUDE_DIRS "."
    REQUIRES esp_tinyusb driver esp_timer
)
