/* Internal to the script component. */
#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t script_store_init(void);
/* Script task only: */
const char *script_store_map(size_t *len);
esp_err_t script_store_write(const char *src, size_t len);
void script_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void script_console_write(const char *buf, size_t len);
const char *script_example(void);
