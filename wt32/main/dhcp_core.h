/* DHCP server logic for the own-network mode (RFC 2131/2132 subset).
 *
 * - Pool of addresses in the WT32's /24; leases remembered by MAC, so a
 *   client keeps its address; reservations pin a MAC to an address (inside
 *   or outside the pool).
 * - DISCOVER -> OFFER, REQUEST (selecting, init-reboot, renewing,
 *   rebinding) -> ACK / NAK, RELEASE, DECLINE, INFORM. Relayed requests
 *   (giaddr set) are ignored: there is no relay in this network.
 * - Replies are broadcast unless the client has an address (ciaddr).
 * - Leases survive restarts through dhcp_core_save()/dhcp_core_load()
 *   (remaining time, since there is no wall clock).
 *
 * Pure C, no ESP-IDF headers: host-testable. Addresses are in network byte
 * order, times in seconds of a monotonic clock supplied by the caller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DHCP_MAX_LEASES   32
#define DHCP_HOST_LEN     24
#define DHCP_DECLINED_S   600       /* an address a client found in use */

typedef struct {
    bool used;
    bool reserved;                  /* pinned: never expires or moves */
    bool bound;                     /* ACKed at least once (else just offered) */
    uint8_t mac[6];
    uint32_t ip;
    uint32_t expires;               /* monotonic seconds; 0 for reservations */
    char host[DHCP_HOST_LEN];       /* option 12 from the client, for the page */
} dhcp_lease_t;

typedef struct {
    uint32_t server_ip;
    uint32_t netmask;
    uint32_t pool_first;
    uint32_t pool_last;
    uint32_t router;                /* 0 = none */
    uint32_t dns;                   /* 0 = none */
    uint32_t lease_s;
    dhcp_lease_t leases[DHCP_MAX_LEASES];
    bool dirty;                     /* leases changed since the last save */
} dhcp_core_t;

typedef enum {
    DHCP_REPLY_NONE,
    DHCP_REPLY_BROADCAST,           /* to 255.255.255.255:68 */
    DHCP_REPLY_UNICAST,             /* to *dst:68 */
} dhcp_reply_t;

void dhcp_core_init(dhcp_core_t *d, uint32_t server_ip, uint32_t netmask,
                    uint32_t pool_first, uint32_t pool_last, uint32_t lease_s);

/* One request in, at most one reply out (resp must hold 576 bytes). */
dhcp_reply_t dhcp_core_handle(dhcp_core_t *d, const uint8_t *req, size_t len,
                              uint8_t *resp, size_t *resp_len, uint32_t *dst, uint32_t now);

/* Reservations. The address must be in the subnet, not the server's, and
 * not reserved for another MAC; a client already using it elsewhere moves
 * at its next renewal (it gets a NAK). */
bool dhcp_core_reserve(dhcp_core_t *d, const uint8_t mac[6], uint32_t ip);
bool dhcp_core_unreserve(dhcp_core_t *d, const uint8_t mac[6]);

/* Current address of a MAC (bound lease or reservation), 0 if none. */
uint32_t dhcp_core_ip_of(const dhcp_core_t *d, const uint8_t mac[6], uint32_t now);

/* Persistence: a flat buffer; load skips entries that no longer fit the
 * subnet. Returns bytes written / whether the buffer was accepted. */
size_t dhcp_core_save(const dhcp_core_t *d, uint8_t *buf, size_t cap, uint32_t now);
bool dhcp_core_load(dhcp_core_t *d, const uint8_t *buf, size_t len, uint32_t now);

/* Probe for another DHCP server (access-point mode, while the WT32 runs on
 * its fallback address): a DISCOVER with the broadcast flag, and a check
 * that a reply is an OFFER to it (same xid and chaddr, a yiaddr, a server
 * id other than own_ip). The probe never takes the offered address. */
size_t dhcp_probe_build(uint8_t *buf, size_t cap, const uint8_t mac[6], uint32_t xid);
bool dhcp_probe_is_offer(const uint8_t *p, size_t len, const uint8_t mac[6], uint32_t xid,
                         uint32_t own_ip, uint32_t *server_id);
