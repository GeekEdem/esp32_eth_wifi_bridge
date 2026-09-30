/* Frame rewriting for the station <-> single device bridge. See l2rewrite.h. */
#include "l2rewrite.h"

#include <string.h>

#define ETH_HDR         14
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV6  0x86DD
#define IPPROTO_UDP_    17
#define DHCP_SERVER     67
#define DHCP_CLIENT     68
#define BOOTP_FIXED     236     /* up to the magic cookie */
#define BOOTP_CHADDR    28
#define BOOTP_HLEN      2
#define DHCP_OPT_CLIENT_ID 61

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

static inline bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

void l2rw_init(l2rw_t *st, const uint8_t sta_mac[6], bool forward_ipv6)
{
    memset(st, 0, sizeof(*st));
    memcpy(st->sta_mac, sta_mac, 6);
    st->forward_ipv6 = forward_ipv6;
}

void l2rw_forget(l2rw_t *st)
{
    st->dev_known = false;
    memset(st->dev_mac, 0, 6);
    st->dev_ip = 0;
    st->lease_ip = st->lease_mask = st->lease_gw = st->lease_dns = 0;
}

/* UDP checksum over the pseudo header and the datagram; udp_len is trusted
 * to fit the buffer by the caller. */
static uint16_t udp_checksum(const uint8_t *ip, const uint8_t *udp, size_t udp_len)
{
    uint32_t sum = 0;
    for (int i = 12; i < 20; i += 2) {        /* source and destination */
        sum += rd16(ip + i);
    }
    sum += IPPROTO_UDP_;
    sum += udp_len;
    for (size_t i = 0; i + 1 < udp_len; i += 2) {
        sum += (i == 6) ? 0 : rd16(udp + i);  /* checksum field counts as 0 */
    }
    if (udp_len & 1) {
        sum += udp[udp_len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t c = ~sum & 0xFFFF;
    return c ? c : 0xFFFF;
}

/* Swap `from` for `to` in chaddr and in option 61 (type 1 = Ethernet). */
static bool dhcp_swap(uint8_t *ip, uint8_t *udp, size_t udp_len, const uint8_t from[6], const uint8_t to[6])
{
    static const uint8_t cookie[4] = { 0x63, 0x82, 0x53, 0x63 };
    uint8_t *bootp = udp + 8;
    uint8_t *end = udp + udp_len;
    if (udp_len < 8 + BOOTP_FIXED + 4 || memcmp(bootp + BOOTP_FIXED, cookie, 4) != 0) {
        return false;
    }
    bool changed = false;
    if (bootp[BOOTP_HLEN] == 6 && mac_eq(bootp + BOOTP_CHADDR, from)) {
        memcpy(bootp + BOOTP_CHADDR, to, 6);
        changed = true;
    }
    /* Servers echo option 61 (RFC 6842), so it is swapped both ways. */
    uint8_t *p = bootp + BOOTP_FIXED + 4;
    while (p < end && *p != 0xFF) {
        if (*p == 0) {                          /* pad */
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end) {
            break;
        }
        if (p[0] == DHCP_OPT_CLIENT_ID && p[1] == 7 && p[2] == 1 && mac_eq(p + 3, from)) {
            memcpy(p + 3, to, 6);
            changed = true;
        }
        p += 2 + p[1];
    }
    if (changed && rd16(udp + 6) != 0) {        /* 0 = no checksum (IPv4) */
        wr16(udp + 6, udp_checksum(ip, udp, udp_len));
    }
    return changed;
}

/* Remember address, mask, router and DNS from a DHCP ACK. */
static void dhcp_snoop_ack(l2rw_t *st, const uint8_t *udp, size_t udp_len)
{
    const uint8_t *bootp = udp + 8;
    const uint8_t *end = udp + udp_len;
    if (udp_len < 8 + BOOTP_FIXED + 4) {
        return;
    }
    uint32_t mask = 0, gw = 0, dns = 0;
    bool ack = false;
    const uint8_t *p = bootp + BOOTP_FIXED + 4;
    while (p < end && *p != 0xFF) {
        if (*p == 0) {
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end) {
            return;
        }
        if (p[0] == 53 && p[1] == 1) {
            ack = p[2] == 5;
        } else if (p[0] == 1 && p[1] == 4) {
            memcpy(&mask, p + 2, 4);
        } else if (p[0] == 3 && p[1] >= 4) {
            memcpy(&gw, p + 2, 4);
        } else if (p[0] == 6 && p[1] >= 4) {
            memcpy(&dns, p + 2, 4);
        }
        p += 2 + p[1];
    }
    uint32_t yiaddr;
    memcpy(&yiaddr, bootp + 16, 4);
    if (ack && yiaddr) {
        st->lease_ip = yiaddr;
        st->lease_mask = mask;
        st->lease_gw = gw;
        st->lease_dns = dns;
    }
}

/* Returns the UDP header of an unfragmented IPv4/UDP packet with the given
 * ports, or NULL. *udp_len is the datagram length, bounded by the frame. */
static uint8_t *ipv4_udp(uint8_t *frame, size_t len, uint16_t sport, uint16_t dport, size_t *udp_len)
{
    uint8_t *ip = frame + ETH_HDR;
    if (len < ETH_HDR + 20 || (ip[0] >> 4) != 4) {
        return NULL;
    }
    size_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < ETH_HDR + ihl + 8 || ip[9] != IPPROTO_UDP_) {
        return NULL;
    }
    if (rd16(ip + 6) & 0x3FFF) {                /* MF flag or fragment offset */
        return NULL;
    }
    uint8_t *udp = ip + ihl;
    if (rd16(udp) != sport || rd16(udp + 2) != dport) {
        return NULL;
    }
    size_t ulen = rd16(udp + 4);
    if (ulen < 8 || udp + ulen > frame + len) {
        return NULL;
    }
    *udp_len = ulen;
    return udp;
}

l2rw_verdict_t l2rw_from_wired(l2rw_t *st, uint8_t *frame, size_t len)
{
    if (len < ETH_HDR) {
        return L2RW_DROP;
    }
    uint8_t *src = frame + 6;
    static const uint8_t zero[6];
    if ((src[0] & 0x01) || mac_eq(src, zero)) {
        return L2RW_DROP;                       /* not a valid unicast source */
    }
    if (!st->dev_known) {
        memcpy(st->dev_mac, src, 6);
        st->dev_known = true;
    } else if (!mac_eq(src, st->dev_mac)) {
        st->foreign_frames++;                   /* one device only */
        return L2RW_DROP;
    }

    uint16_t type = rd16(frame + 12);
    if (type == ETHERTYPE_IPV6 && !st->forward_ipv6) {
        st->ipv6_dropped++;
        return L2RW_DROP;
    }
    memcpy(src, st->sta_mac, 6);

    if (type == ETHERTYPE_ARP && len >= ETH_HDR + 28) {
        uint8_t *arp = frame + ETH_HDR;
        if (mac_eq(arp + 8, st->dev_mac)) {     /* sender hardware address */
            memcpy(arp + 8, st->sta_mac, 6);
        }
        uint32_t spa;
        memcpy(&spa, arp + 14, 4);
        if (spa) {
            st->dev_ip = spa;
        }
    } else if (type == ETHERTYPE_IPV4 && len >= ETH_HDR + 20) {
        uint32_t sip;
        memcpy(&sip, frame + ETH_HDR + 12, 4);
        if (sip) {
            st->dev_ip = sip;
        }
        size_t ulen;
        uint8_t *udp = ipv4_udp(frame, len, DHCP_CLIENT, DHCP_SERVER, &ulen);
        if (udp && dhcp_swap(frame + ETH_HDR, udp, ulen, st->dev_mac, st->sta_mac)) {
            st->dhcp_rewrites++;
        }
    }
    return L2RW_FORWARD;
}

l2rw_verdict_t l2rw_to_wired(l2rw_t *st, uint8_t *frame, size_t len)
{
    if (len < ETH_HDR) {
        return L2RW_DROP;
    }
    uint16_t type = rd16(frame + 12);
    if (type == ETHERTYPE_IPV6 && !st->forward_ipv6) {
        st->ipv6_dropped++;
        return L2RW_DROP;
    }
    if (!st->dev_known) {
        return L2RW_FORWARD;                    /* broadcasts still help the device boot */
    }
    if (mac_eq(frame, st->sta_mac)) {
        memcpy(frame, st->dev_mac, 6);
    }
    if (type == ETHERTYPE_ARP && len >= ETH_HDR + 28) {
        uint8_t *arp = frame + ETH_HDR;
        if (mac_eq(arp + 18, st->sta_mac)) {    /* target hardware address */
            memcpy(arp + 18, st->dev_mac, 6);
        }
    } else if (type == ETHERTYPE_IPV4) {
        size_t ulen;
        uint8_t *udp = ipv4_udp(frame, len, DHCP_SERVER, DHCP_CLIENT, &ulen);
        if (udp) {
            if (dhcp_swap(frame + ETH_HDR, udp, ulen, st->sta_mac, st->dev_mac)) {
                st->dhcp_rewrites++;
            }
            if (ulen >= 8 + BOOTP_FIXED + 4 && mac_eq(udp + 8 + BOOTP_CHADDR, st->dev_mac)) {
                dhcp_snoop_ack(st, udp, ulen);
            }
        }
    }
    return L2RW_FORWARD;
}
