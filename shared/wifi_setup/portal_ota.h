/* Firmware update through the web page (internal to setup_portal).
 *
 * POST /api/ota with the application image (build/<project>.bin, not the
 * merged image for 0x0) as the raw body. The image must be for this chip
 * and this project. After the restart the new image is on probation: it is
 * confirmed after PORTAL_OTA_CONFIRM_S seconds of running with the web
 * server up; if it resets before that, the bootloader goes back to the
 * previous one (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define PORTAL_OTA_CONFIRM_S 60

/* Arms the confirmation timer if the running image is on probation. */
void portal_ota_init(void);

esp_err_t portal_ota_post(httpd_req_t *req);

/* ",\"ota\":{...}" for /api/status */
size_t portal_ota_status(char *buf, size_t pos, size_t cap);
