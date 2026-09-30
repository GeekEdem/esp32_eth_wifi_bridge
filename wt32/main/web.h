/* WT32 web page: shared setup portal (Wi-Fi, password) plus mode, own
 * network settings and per-mode status. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "settings.h"

/* setup_boot: this start is the one-time setup start from the button (the
 * page says so); s->mode is then the client mode for this boot only. */
esp_err_t web_start(const wt32_settings_t *s, bool setup_boot);

/* ",\"key\":value" pairs of the device status (page and script status()). */
size_t web_status_extra(char *buf, size_t pos, size_t cap);
