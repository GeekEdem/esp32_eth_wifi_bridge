/* Frame rewriting for a Wi-Fi station <-> single Ethernet device L2 bridge.
 *
 * An 802.11 station may only use its own MAC, so frames from the device
 * leave with the station MAC and frames for the station MAC are handed to
 * the device. MACs carried inside ARP and DHCP (chaddr, client-id option 61)
 * are swapped as well, so the home router sees one consistent client.
 *
 * Pure functions over a state struct: no ESP-IDF dependencies, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t sta_mac[6];
    bool forward_ipv6;

    /* learned from the wired side */
    bool dev_known;
    uint8_t dev_mac[6];
    uint32_t dev_ip;            /* last IPv4 source seen from the device, network order */

    /* from the last DHCP ACK the device received (network order, 0 = none) */
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
l2rw_verdict_t l2rw_from_wired(l2rw_t *st, uint8_t *frame, size_t len);

/* Wi-Fi -> device. */
l2rw_verdict_t l2rw_to_wired(l2rw_t *st, uint8_t *frame, size_t len);
