/* Target UART <-> RFC2217 (network) and USB-Serial/JTAG (fixed baud). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t bridge_start(void);

bool bridge_client_connected(void);

/* Bytes moved in either direction; the LED task watches it for activity. */
uint32_t bridge_activity(void);
