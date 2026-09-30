idf_component_register(
    SRCS "wifi_setup.c" "setup_portal.c" "portal_auth.c" "portal_ota.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "auth.js" "ota.js"
    REQUIRES esp_wifi esp_http_server
    PRIV_REQUIRES esp_netif esp_event esp_timer esp_app_format esp_hw_support nvs_flash lwip mbedtls
                  app_update bootloader_support)

# Source commit of this build, for the status page. Taken when CMake configures
# (build_firmware.sh always builds from scratch), "-dirty" with local changes.
execute_process(COMMAND git describe --always --dirty --abbrev=10
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE portal_git_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE portal_git_result)
if(NOT portal_git_result EQUAL 0 OR portal_git_commit STREQUAL "")
    set(portal_git_commit "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PORTAL_GIT_COMMIT="${portal_git_commit}")
