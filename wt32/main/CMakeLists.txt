idf_component_register(
    SRCS "main.c" "settings.c" "eth.c" "client_mode.c" "own_mode.c" "dhcp_core.c" "dhcp_server.c" "l2rewrite.c" "mgmt_demux.c" "web.c"
         "display.c" "disp_ui.c" "font6x10.c" "button.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "page.html"
    PRIV_REQUIRES wifi_setup script_berry esp_wifi esp_eth esp_netif esp_event esp_timer esp_http_server
                  nvs_flash esp_driver_gpio esp_driver_i2c esp_app_format lwip)

# Page languages: wt32/main/i18n, script_berry/i18n and wifi_setup/i18n
idf_component_get_property(script_dir script_berry COMPONENT_DIR)
portal_i18n(${COMPONENT_DIR}/i18n ${script_dir}/i18n)
