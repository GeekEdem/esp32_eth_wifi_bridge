/* Host test for mgmt_demux.c:
 *   cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dmx mgmt_demux_test.c ../main/mgmt_demux.c && /tmp/dmx
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mgmt_demux.h"

#define MGMT 28480

static const uint8_t STA[6] = { 0x24, 0x6f, 0x28, 1, 2, 3 };
static const uint8_t PEER[6] = { 0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcc };
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t MCAST[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };

static uint32_t ip4(int a, int b, int c, int d)
{
    uint8_t x[4] = { a, b, c, d };
    uint32_t v;
    memcpy(&v, x, 4);
    return v;
}


/* Build Ethernet + IPv4 + TCP/UDP. flags only for TCP. */
static size_t pkt(uint8_t *f, const uint8_t *dst, const uint8_t *src, uint8_t proto, uint32_t sip, uint32_t dip,
                  uint16_t sport, uint16_t dport, uint8_t flags)
{
    memset(f, 0, 128);
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x08;
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = proto;
    memcpy(ip + 12, &sip, 4);
    memcpy(ip + 16, &dip, 4);
    uint8_t *l4 = ip + 20;
    l4[0] = sport >> 8; l4[1] = sport & 0xff;
    l4[2] = dport >> 8; l4[3] = dport & 0xff;
    size_t l4len = proto == 6 ? 20 : 8;
    if (proto == 6) {
        l4[12] = 0x50;
        l4[13] = flags;
    } else {
        l4[4] = 0; l4[5] = 8;
    }
    size_t iplen = 20 + l4len;
    ip[2] = iplen >> 8; ip[3] = iplen & 0xff;
    return 14 + iplen;
}

#define SYN 0x02
#define ACK 0x10
#define FIN 0x01
#define RST 0x04

int main(void)
{
    demux_t d;
    uint8_t f[128];
    size_t n;
    const uint32_t dev = ip4(192, 168, 1, 50), pc = ip4(192, 168, 1, 10), dns = ip4(8, 8, 8, 8);
    uint32_t t = 1000;

    demux_init(&d, MGMT);

    /* no shared IP yet: unicast goes to the device, broadcast to both */
    n = pkt(f, STA, PEER, 6, pc, dev, 50000, MGMT, SYN);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);
    n = pkt(f, BCAST, PEER, 17, pc, ip4(255, 255, 255, 255), 68, 67, 0);
    assert(demux_inbound(&d, f, n, t) == DEMUX_BOTH);

    demux_set_local_ip(&d, dev);

    /* 1. new connection to the management port -> WT32, and the whole flow */
    n = pkt(f, STA, PEER, 6, pc, dev, 50000, MGMT, SYN);
    assert(demux_inbound(&d, f, n, t) == DEMUX_LOCAL);
    n = pkt(f, STA, STA, 6, dev, pc, MGMT, 50000, SYN | ACK);    /* WT32's SYN-ACK */
    demux_outbound(&d, f, n, t);
    n = pkt(f, STA, PEER, 6, pc, dev, 50000, MGMT, ACK);
    assert(demux_inbound(&d, f, n, t + 10) == DEMUX_LOCAL);
    assert(demux_active_flows(&d, t + 10) == 1);

    /* 2. printing: TCP 9100 -> device */
    n = pkt(f, STA, PEER, 6, pc, dev, 50001, 9100, SYN);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);
    n = pkt(f, STA, PEER, 6, pc, dev, 50001, 9100, ACK);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);

    /* 3. device's own outbound flow from local port 28480: SYN-ACK and data stay with the device */
    n = pkt(f, STA, PEER, 6, pc, dev, 443, MGMT, SYN | ACK);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);
    n = pkt(f, STA, PEER, 6, pc, dev, 443, MGMT, ACK);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);
    n = pkt(f, STA, PEER, 6, pc, dev, 443, MGMT, RST);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);

    /* 4. other destination IP, UDP to the management port, ICMP -> device */
    n = pkt(f, STA, PEER, 6, pc, ip4(192, 168, 1, 99), 50002, MGMT, SYN);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);
    n = pkt(f, STA, PEER, 17, pc, dev, 50003, MGMT, 0);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);
    n = pkt(f, STA, PEER, 1, pc, dev, 0, 0, 0);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);

    /* 5. ARP (unicast reply too) and multicast -> both */
    memset(f, 0, 64);
    memcpy(f, STA, 6); memcpy(f + 6, PEER, 6); f[12] = 0x08; f[13] = 0x06;
    assert(demux_inbound(&d, f, 60, t) == DEMUX_BOTH);
    n = pkt(f, MCAST, PEER, 17, pc, ip4(224, 0, 0, 251), 5353, 5353, 0);
    assert(demux_inbound(&d, f, n, t) == DEMUX_BOTH);

    /* 6. WT32-initiated UDP (DNS): the reply comes back to it, other ports do not */
    n = pkt(f, PEER, STA, 17, dev, dns, 61000, 53, 0);
    demux_outbound(&d, f, n, t);
    n = pkt(f, STA, PEER, 17, dns, dev, 53, 61000, 0);
    assert(demux_inbound(&d, f, n, t + 5) == DEMUX_LOCAL);
    n = pkt(f, STA, PEER, 17, dns, dev, 53, 61001, 0);
    assert(demux_inbound(&d, f, n, t + 5) == DEMUX_DEVICE);

    /* 7. unicast mDNS answers from the WT32 do not open a flow */
    n = pkt(f, PEER, STA, 17, dev, pc, 5353, 5353, 0);
    demux_outbound(&d, f, n, t);
    n = pkt(f, STA, PEER, 17, pc, dev, 5353, 5353, 0);
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);

    /* 8. expiry: UDP after 60 s, TCP after FIN + 10 s */
    n = pkt(f, STA, PEER, 17, dns, dev, 53, 61000, 0);
    assert(demux_inbound(&d, f, n, t + 5 + DEMUX_UDP_IDLE_MS + 1) == DEMUX_DEVICE);
    n = pkt(f, STA, PEER, 6, pc, dev, 50000, MGMT, FIN | ACK);
    assert(demux_inbound(&d, f, n, t + 20) == DEMUX_LOCAL);
    n = pkt(f, STA, PEER, 6, pc, dev, 50000, MGMT, ACK);
    assert(demux_inbound(&d, f, n, t + 25) == DEMUX_LOCAL);
    assert(demux_inbound(&d, f, n, t + 25 + DEMUX_CLOSING_MS + 1) == DEMUX_DEVICE);

    /* 9. non-first fragment -> device even if it would match */
    n = pkt(f, STA, PEER, 6, pc, dev, 50010, MGMT, SYN);
    f[14 + 6] = 0x00; f[14 + 7] = 0x10;         /* fragment offset 16 */
    assert(demux_inbound(&d, f, n, t) == DEMUX_DEVICE);

    /* 10. table full: 20 new management connections, the newest are kept */
    for (int i = 0; i < 20; i++) {
        n = pkt(f, STA, PEER, 6, pc, dev, 40000 + i, MGMT, SYN);
        assert(demux_inbound(&d, f, n, t + 100 + i) == DEMUX_LOCAL);
    }
    assert(demux_active_flows(&d, t + 200) == DEMUX_MAX_FLOWS);
    assert(d.evictions > 0);
    n = pkt(f, STA, PEER, 6, pc, dev, 40019, MGMT, ACK);
    assert(demux_inbound(&d, f, n, t + 200) == DEMUX_LOCAL);
    n = pkt(f, STA, PEER, 6, pc, dev, 40000, MGMT, ACK);
    assert(demux_inbound(&d, f, n, t + 200) == DEMUX_DEVICE);

    /* 11. address change drops flows */
    demux_set_local_ip(&d, ip4(192, 168, 1, 51));
    assert(demux_active_flows(&d, t + 200) == 0);

    /* 12. a slightly older timestamp (clock sampled before the lock) keeps the flow */
    demux_set_local_ip(&d, dev);
    n = pkt(f, STA, PEER, 6, pc, dev, 46000, MGMT, SYN);
    assert(demux_inbound(&d, f, n, t + 300) == DEMUX_LOCAL);
    n = pkt(f, STA, PEER, 6, pc, dev, 46000, MGMT, ACK);
    assert(demux_inbound(&d, f, n, t + 297) == DEMUX_LOCAL);

    /* 13. timer wrap-around does not expire a fresh flow */
    uint32_t w = 0xFFFFFF00u;
    n = pkt(f, STA, PEER, 6, pc, dev, 45000, MGMT, SYN);
    assert(demux_inbound(&d, f, n, w) == DEMUX_LOCAL);
    n = pkt(f, STA, PEER, 6, pc, dev, 45000, MGMT, ACK);
    assert(demux_inbound(&d, f, n, w + 0x200) == DEMUX_LOCAL);

    /* 14. fuzz: random and truncated frames */
    srand(7);
    for (int i = 0; i < 300000; i++) {
        size_t len = rand() % 130;
        uint8_t *buf = malloc(len ? len : 1);
        if (i % 2) {
            size_t m = pkt(f, (rand() & 1) ? STA : BCAST, PEER, (rand() & 1) ? 6 : 17, pc, dev,
                           rand(), (rand() & 3) ? MGMT : rand(), rand());
            if (len > m) len = m;
            memcpy(buf, f, len);
            if (len) buf[rand() % len] ^= rand();
        } else {
            for (size_t k = 0; k < len; k++) buf[k] = rand();
        }
        if (rand() & 1) demux_inbound(&d, buf, len, rand());
        else demux_outbound(&d, buf, len, rand());
        free(buf);
    }

    puts("all mgmt_demux tests passed");
    return 0;
}
