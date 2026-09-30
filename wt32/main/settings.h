/* Persistent settings of the WT32 app (NVS namespace "wt32"). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WT32_MODE_CLIENT = 0,   /* joins an existing Wi-Fi, transparent for the device */
    WT32_MODE_OWN = 1,      /* "router": own access point + Ethernet in one network, own DHCP */
    WT32_MODE_AP = 2,       /* access point for the network on the cable (its router's DHCP) */
} wt32_mode_t;

typedef struct {
    wt32_mode_t mode;
    char own_ssid[33];
    char own_pass[65];      /* 8-63 characters */
    uint8_t own_channel;    /* 1-13 */
    char own_ip[16];        /* WT32 address, /24 (access point: fallback address) */
} wt32_settings_t;

void settings_load(wt32_settings_t *s);
/* The access point settings (own_*) are complete: needed by both AP modes. */
bool settings_own_valid(const wt32_settings_t *s);

esp_err_t settings_save_mode(wt32_mode_t mode);
esp_err_t settings_save_own(const char *ssid, const char *pass, uint8_t channel, const char *ip);

/* Validation shared with the web handlers. */
bool settings_ip_ok(const char *ip);
