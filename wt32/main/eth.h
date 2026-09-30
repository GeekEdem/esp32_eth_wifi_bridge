/* WT32-ETH01 Ethernet: internal EMAC + LAN8720, 50 MHz clock in on GPIO0. */
#pragma once

#include "esp_err.h"
#include "esp_eth.h"

/* Installs the driver; the mode decides the input path and starts it. */
esp_err_t eth_init(esp_eth_handle_t *out);

/* Start and switch to promiscuous mode (both modes bridge frames). */
esp_err_t eth_start_promiscuous(esp_eth_handle_t eth);

/* What the PHY negotiated with the device, updated on link up (logged too). */
typedef struct {
    bool up;
    int speed_mbps;             /* 10 or 100 */
    bool full_duplex;
    bool pause;                 /* the device takes PAUSE frames: flow control is active */
} wt32_eth_link_t;

void eth_link_get(wt32_eth_link_t *out);
