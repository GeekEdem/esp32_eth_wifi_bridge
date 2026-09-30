/* Who gets a frame arriving from Wi-Fi: the device, the WT32 itself, or both.
 *
 * In the transparent mode the WT32 shares the device's IP (and the station
 * MAC). It keeps only its own traffic:
 *   - new TCP connections (SYN without ACK) to the management port;
 *   - packets of flows the WT32 itself has open (inbound management
 *     connections and anything it sent out), keyed by protocol, remote
 *     address/port and local port;
 *   - broadcasts, multicasts and ARP go to both.
 * Everything else, including TCP to the management port that is not a new
 * connection (the device's own flow that happens to use that port), goes to
 * the device.
 *
 * Pure C, no ESP-IDF dependencies; the caller serialises access.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEMUX_MAX_FLOWS 16

typedef enum {
    DEMUX_DEVICE,
    DEMUX_LOCAL,
    DEMUX_BOTH,
} demux_target_t;

typedef struct {
    bool used;
    bool closing;               /* FIN or RST seen */
    uint8_t proto;              /* 6 TCP, 17 UDP */
    uint32_t remote_ip;         /* network order */
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t last_ms;
} demux_flow_t;

typedef struct {
    uint16_t mgmt_port;
    uint32_t local_ip;          /* shared IP, network order; 0 = unknown */
    demux_flow_t flows[DEMUX_MAX_FLOWS];
    uint32_t evictions;
} demux_t;

/* Idle timeouts */
#define DEMUX_TCP_IDLE_MS     (5 * 60 * 1000)
#define DEMUX_CLOSING_MS      (10 * 1000)
#define DEMUX_UDP_IDLE_MS     (60 * 1000)

void demux_init(demux_t *d, uint16_t mgmt_port);
void demux_set_local_ip(demux_t *d, uint32_t ip);

/* Frame from Wi-Fi, before any MAC rewriting. */
demux_target_t demux_inbound(demux_t *d, const uint8_t *frame, size_t len, uint32_t now_ms);

/* Frame the WT32 itself sends; opens or refreshes its flow. */
void demux_outbound(demux_t *d, const uint8_t *frame, size_t len, uint32_t now_ms);

int demux_active_flows(const demux_t *d, uint32_t now_ms);
