idf_component_register(
    SRCS "wifi_setup.c" "setup_portal.c" "portal_auth.c" "portal_ota.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "auth.js" "ota.js"
    REQUIRES esp_wifi esp_http_server
    PRIV_REQUIRES esp_netif esp_event esp_timer esp_app_format esp_hw_support nvs_flash lwip mbedtls
                  app_update bootloader_support)
