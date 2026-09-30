/* Client mode: Ethernet <-> Wi-Fi station L2 bridge for one device, plus the
 * WT32's own management traffic on the device's IP (see mgmt_demux.h). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_eth.h"

typedef struct {
    bool wifi_up;
    bool eth_up;
    bool dev_known;
    uint8_t dev_mac[6];
    uint32_t dev_ip;            /* network order, 0 = unknown */
    uint32_t to_wifi_frames, to_wifi_bytes;
    uint32_t to_eth_frames, to_eth_bytes;
    uint32_t drop_wifi_down, drop_eth_down;
    uint32_t tx_err_wifi, tx_err_eth;
    uint32_t foreign_frames, ipv6_dropped, dhcp_rewrites;
    uint32_t mgmt_ip;           /* shared management address, 0 = none yet */
    uint32_t mgmt_rx_frames, mgmt_tx_frames, mgmt_tx_err;
    int mgmt_flows;
    uint32_t mgmt_evictions;
} client_stats_t;

/* Hooks Ethernet and the station; call before wifi_setup_start(). */
esp_err_t client_mode_start(esp_eth_handle_t eth);

void client_mode_get_stats(client_stats_t *out);

/* Forget the learned device, e.g. after swapping it. */
void client_mode_relearn(void);
