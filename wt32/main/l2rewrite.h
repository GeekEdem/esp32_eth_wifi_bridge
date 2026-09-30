/* Frame rewriting for a Wi-Fi station <-> single Ethernet device L2 bridge.
 *
 * An 802.11 station may only use its own MAC, so frames from the device
 * leave with the station MAC and frames for the station MAC are handed to
 * the device. MACs carried inside ARP and DHCP (chaddr, client-id option 61)
 * are swapped as well, so the home router sees one consistent client.
 *
 * It also keeps the device's address for the WT32's management interface
 * (which shares it): the address from the device's DHCP lease if it has one,
 * otherwise the static address it sends from. A static address changes only
 * after the old one has been silent for L2RW_IP_SWITCH_MS while the device
 * uses another, so aliases and bursts from a second address do not move it.
 * The address counts as reachable from the Wi-Fi network if it came from DHCP
 * or another host of its subnet has been heard on Wi-Fi since the address was
 * taken (a static address from a different network never is).
 *
 * Pure functions over a state struct: no ESP-IDF dependencies, host-testable.
 * Times are milliseconds from any monotonic clock.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define L2RW_IP_SWITCH_MS   30000u

typedef struct {
    uint8_t sta_mac[6];
    bool forward_ipv6;

    /* learned from the wired side */
    bool dev_known;
    uint8_t dev_mac[6];
    uint32_t dev_ip;            /* the device's address (see above), network order, 0 = unknown */
    uint32_t dev_ip_seen_ms;    /* last frame from dev_ip */
    uint32_t cand_ip;           /* another source address the device is using */
    bool net_seen;              /* another host of dev_ip's subnet heard on Wi-Fi */

    /* from the last DHCP ACK the device received (network order, 0 = none);
     * dropped when the device stops using that address */
    uint32_t lease_ip;
    uint32_t lease_mask;
    uint32_t lease_gw;
    uint32_t lease_dns;

    /* counters */
    uint32_t foreign_frames;    /* from a second MAC on the wire (dropped) */
    uint32_t ipv6_dropped;
    uint32_t dhcp_rewrites;
} l2rw_t;

typedef enum {
    L2RW_FORWARD,
    L2RW_DROP,
} l2rw_verdict_t;

void l2rw_init(l2rw_t *st, const uint8_t sta_mac[6], bool forward_ipv6);

/* Forget the device (link down, device swapped). */
void l2rw_forget(l2rw_t *st);  /* also drops the lease */

/* Device -> Wi-Fi. Learns the device MAC from the first valid frame. */
l2rw_verdict_t l2rw_from_wired(l2rw_t *st, uint8_t *frame, size_t len, uint32_t now_ms);

/* Wi-Fi -> device. */
l2rw_verdict_t l2rw_to_wired(l2rw_t *st, uint8_t *frame, size_t len, uint32_t now_ms);

/* Address for the management interface (network order; ip 0 = none yet). */
typedef struct {
    uint32_t ip, mask, gw, dns;
    bool from_lease;
    bool reachable;             /* from DHCP, or its subnet is present on Wi-Fi */
} l2rw_addr_t;

void l2rw_mgmt_addr(const l2rw_t *st, l2rw_addr_t *out);
