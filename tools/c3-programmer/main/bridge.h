/* Target UART <-> RFC2217 (network) and USB-Serial/JTAG (fixed baud). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t bridge_start(void);

bool bridge_client_connected(void);

/* For diagnosis (/api/status): seconds since the current client finished the
 * RFC2217 negotiation and since the last drop of a silent client (-1 = none),
 * and the number of such drops since start. */
typedef struct {
    int32_t connected_s;
    int32_t dropped_s;
    uint32_t drops;
} bridge_client_info_t;
void bridge_client_info(bridge_client_info_t *info);

/* Bytes moved in either direction; the LED task watches it for activity. */
uint32_t bridge_activity(void);
