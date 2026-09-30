idf_component_register(
    SRCS "main.c" "target_ctl.c" "bridge.c" "web.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "portal.html"
    PRIV_REQUIRES wifi_setup esp_timer esp_http_server nvs_flash esp_netif esp_event
                  esp_driver_uart esp_driver_gpio esp_driver_usb_serial_jtag)
