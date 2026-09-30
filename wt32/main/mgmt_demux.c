/* Frame demultiplexing between the device and the WT32. See mgmt_demux.h. */
#include "mgmt_demux.h"

#include <string.h>

#define ETH_HDR         14
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define PROTO_TCP       6
#define PROTO_UDP       17
#define TCP_FIN         0x01
#define TCP_SYN         0x02
#define TCP_RST         0x04
#define TCP_ACK         0x10

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

void demux_init(demux_t *d, uint16_t mgmt_port)
{
    memset(d, 0, sizeof(*d));
    d->mgmt_port = mgmt_port;
}

void demux_set_local_ip(demux_t *d, uint32_t ip)
{
    if (ip != d->local_ip) {
        /* Flows were bound to the old address. */
        memset(d->flows, 0, sizeof(d->flows));
        d->local_ip = ip;
    }
}

static bool flow_expired(const demux_flow_t *f, uint32_t now_ms)
{
    /* Callers sample the clock before taking the lock, so "now" can be a
     * little older than last_ms; that is no idle time, not a huge one. */
    int32_t idle = (int32_t)(now_ms - f->last_ms);
    if (idle < 0) {
        return false;
    }
    if (f->closing) {
        return idle > DEMUX_CLOSING_MS;
    }
    return idle > (f->proto == PROTO_TCP ? DEMUX_TCP_IDLE_MS : DEMUX_UDP_IDLE_MS);
}

static demux_flow_t *flow_find(demux_t *d, uint8_t proto, uint32_t rip, uint16_t rport, uint16_t lport,
                               uint32_t now_ms)
{
    for (int i = 0; i < DEMUX_MAX_FLOWS; i++) {
        demux_flow_t *f = &d->flows[i];
        if (!f->used) {
            continue;
        }
        if (flow_expired(f, now_ms)) {
            f->used = false;
            continue;
        }
        if (f->proto == proto && f->remote_ip == rip && f->remote_port == rport && f->local_port == lport) {
            return f;
        }
    }
    return NULL;
}

static demux_flow_t *flow_add(demux_t *d, uint8_t proto, uint32_t rip, uint16_t rport, uint16_t lport,
                              uint32_t now_ms)
{
    demux_flow_t *slot = NULL;
    for (int i = 0; i < DEMUX_MAX_FLOWS && !slot; i++) {
        if (!d->flows[i].used || flow_expired(&d->flows[i], now_ms)) {
            slot = &d->flows[i];
        }
    }
    if (!slot) {
        /* Full: drop the least recently used, preferring closing flows. */
        for (int i = 0; i < DEMUX_MAX_FLOWS; i++) {
            demux_flow_t *f = &d->flows[i];
            if (!slot || (f->closing && !slot->closing) ||
                (f->closing == slot->closing && (int32_t)(f->last_ms - slot->last_ms) < 0)) {
                slot = f;
            }
        }
        d->evictions++;
    }
    *slot = (demux_flow_t) {
        .used = true,
        .proto = proto,
        .remote_ip = rip,
        .remote_port = rport,
        .local_port = lport,
        .last_ms = now_ms,
    };
    return slot;
}

/* L4 header of an unfragmented (or first-fragment) IPv4 TCP/UDP packet. */
static const uint8_t *ipv4_l4(const uint8_t *frame, size_t len, uint8_t *proto, uint32_t *src, uint32_t *dst)
{
    const uint8_t *ip = frame + ETH_HDR;
    if (len < ETH_HDR + 20 || (ip[0] >> 4) != 4) {
        return NULL;
    }
    size_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < ETH_HDR + ihl + 4) {
        return NULL;
    }
    memcpy(src, ip + 12, 4);
    memcpy(dst, ip + 16, 4);
    *proto = ip[9];
    if (rd16(ip + 6) & 0x1FFF) {
        return NULL;                            /* not the first fragment: no ports */
    }
    if (*proto == PROTO_TCP && len < ETH_HDR + ihl + 14) {
        return NULL;                            /* need the flags byte */
    }
    return ip + ihl;
}

demux_target_t demux_inbound(demux_t *d, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    if (len < ETH_HDR) {
        return DEMUX_DEVICE;
    }
    if (frame[0] & 0x01) {
        return DEMUX_BOTH;                      /* broadcast / multicast */
    }
    uint16_t type = rd16(frame + 12);
    if (type == ETHERTYPE_ARP) {
        return DEMUX_BOTH;
    }
    if (type != ETHERTYPE_IPV4 || d->local_ip == 0) {
        return DEMUX_DEVICE;
    }
    uint8_t proto;
    uint32_t src, dst;
    const uint8_t *l4 = ipv4_l4(frame, len, &proto, &src, &dst);
    if (!l4 || dst != d->local_ip || (proto != PROTO_TCP && proto != PROTO_UDP)) {
        return DEMUX_DEVICE;
    }
    uint16_t sport = rd16(l4);
    uint16_t dport = rd16(l4 + 2);

    demux_flow_t *f = flow_find(d, proto, src, sport, dport, now_ms);
    if (f) {
        f->last_ms = now_ms;
        if (proto == PROTO_TCP && (l4[13] & (TCP_FIN | TCP_RST))) {
            f->closing = true;
        }
        return DEMUX_LOCAL;
    }
    if (proto == PROTO_TCP && dport == d->mgmt_port && (l4[13] & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
        flow_add(d, proto, src, sport, dport, now_ms);
        return DEMUX_LOCAL;
    }
    return DEMUX_DEVICE;
}

void demux_outbound(demux_t *d, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    if (len < ETH_HDR || (frame[0] & 0x01) || rd16(frame + 12) != ETHERTYPE_IPV4) {
        return;
    }
    uint8_t proto;
    uint32_t src, dst;
    const uint8_t *l4 = ipv4_l4(frame, len, &proto, &src, &dst);
    if (!l4 || (proto != PROTO_TCP && proto != PROTO_UDP)) {
        return;
    }
    uint16_t sport = rd16(l4);
    uint16_t dport = rd16(l4 + 2);
    if (proto == PROTO_UDP && sport == 5353) {
        return;     /* unicast mDNS answers: tracking them would steal the device's mDNS */
    }
    demux_flow_t *f = flow_find(d, proto, dst, dport, sport, now_ms);
    if (!f) {
        if (proto == PROTO_TCP && (l4[13] & TCP_RST)) {
            return;                             /* a reset opens nothing */
        }
        f = flow_add(d, proto, dst, dport, sport, now_ms);
    }
    f->last_ms = now_ms;
    if (proto == PROTO_TCP && (l4[13] & (TCP_FIN | TCP_RST))) {
        f->closing = true;
    }
}

int demux_active_flows(const demux_t *d, uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < DEMUX_MAX_FLOWS; i++) {
        if (d->flows[i].used && !flow_expired(&d->flows[i], now_ms)) {
            n++;
        }
    }
    return n;
}
