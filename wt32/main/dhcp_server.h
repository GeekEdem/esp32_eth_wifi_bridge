/* DHCP server task (logic in dhcp_core.c): the router mode's server, and the
 * access-point mode's fallback server for Wi-Fi clients. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t server_ip;             /* addresses in network byte order */
    uint32_t netmask;
    uint32_t pool_first;
    uint32_t pool_last;
    uint32_t lease_s;
    bool persist;                   /* leases and reservations in NVS */
    bool enabled;                   /* answer from the start (see dhcp_server_enable) */
    /* Serve only clients this returns true for (called from the server task);
     * NULL = everyone. */
    bool (*accept)(const uint8_t mac[6]);
} dhcp_server_config_t;

esp_err_t dhcp_server_start(const dhcp_server_config_t *cfg);
/* A disabled server reads and drops requests. */
void dhcp_server_enable(bool on);

uint32_t dhcp_server_ip_of(const uint8_t mac[6]);
bool dhcp_server_reserve(const uint8_t mac[6], uint32_t ip);
bool dhcp_server_unreserve(const uint8_t mac[6]);

/* ",\"dhcp\":{...}" with leases and reservations for /api/status. */
size_t dhcp_server_status_json(char *buf, size_t pos, size_t cap);
