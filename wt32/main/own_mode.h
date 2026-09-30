/* The two access-point modes: access point + Ethernet in one network, with
 * the WT32's DHCP server ("router", WT32_MODE_OWN) or with the DHCP of the
 * network on the cable (WT32_MODE_AP). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_eth.h"
#include "settings.h"

typedef enum {
    UPLINK_NONE,                /* router mode: no uplink */
    UPLINK_WAITING,             /* access point: waiting for DHCP on the cable */
    UPLINK_DHCP,                /* access point: address from the cable's DHCP */
    UPLINK_FALLBACK,            /* access point: no DHCP there, fallback address */
} uplink_t;

typedef struct {
    bool eth_up;
    uplink_t uplink;
    uint32_t gw;                /* access point: gateway from DHCP */
    int ap_clients;
    uint32_t ip;                /* WT32, network order (0 = none yet) */
    char ssid[33];
    bool dev_known;
    uint8_t dev_mac[6];
    uint32_t dev_ip;            /* lease of the Ethernet device, 0 = none yet */
} own_stats_t;

esp_err_t own_mode_start(esp_eth_handle_t eth, const wt32_settings_t *s);
void own_mode_get_stats(own_stats_t *out);
