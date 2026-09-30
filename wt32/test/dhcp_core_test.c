/* Host test for dhcp_core.c: malformed input and save/load robustness under
 * ASan/UBSan (the protocol itself is covered by dhcp_scapy_test.py).
 *   cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dhc dhcp_core_test.c ../main/dhcp_core.c && /tmp/dhc
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhcp_core.h"

static uint32_t ip(int a, int b, int c, int d)
{
    uint8_t x[4] = { a, b, c, d };
    uint32_t v;
    memcpy(&v, x, 4);
    return v;
}

/* A well-formed DISCOVER/REQUEST to mutate. */
static size_t request(uint8_t *p, const uint8_t mac[6], uint8_t type, uint32_t req_ip)
{
    memset(p, 0, 600);
    p[0] = 1; p[1] = 1; p[2] = 6;
    p[4] = 0x12; p[5] = 0x34;
    memcpy(p + 28, mac, 6);
    p[236] = 0x63; p[237] = 0x82; p[238] = 0x53; p[239] = 0x63;
    uint8_t *o = p + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4; }
    *o++ = 12; *o++ = 5; memcpy(o, "abcde", 5); o += 5;
    *o++ = 255;
    return o - p;
}

int main(void)
{
    static dhcp_core_t d;
    dhcp_core_init(&d, ip(192, 168, 77, 1), ip(255, 255, 255, 0), ip(192, 168, 77, 100), ip(192, 168, 77, 110), 7200);
    uint8_t req[600], resp[576], buf[2048];
    size_t rlen;
    uint32_t dst;

    /* fill the table beyond its size: offers stop, nothing breaks */
    int offered = 0;
    for (int i = 0; i < 60; i++) {
        uint8_t mac[6] = { 2, 0, 0, 0, 1, (uint8_t)i };
        size_t n = request(req, mac, 1, 0);
        if (dhcp_core_handle(&d, req, n, resp, &rlen, &dst, 100) != DHCP_REPLY_NONE) {
            assert(rlen >= 300 && rlen <= 576);
            offered++;
        }
    }
    assert(offered == 11);                  /* pool .100-.110 */

    /* fuzz: truncated and corrupted requests, random bytes */
    srand(3);
    for (int i = 0; i < 300000; i++) {
        uint8_t mac[6] = { 2, 0, 0, 0, 2, (uint8_t)(rand() & 0x3F) };
        size_t n = request(req, mac, 1 + rand() % 8, (rand() & 1) ? ip(192, 168, 77, 100 + rand() % 20) : 0);
        if (i % 3 == 0) {
            for (size_t k = 0; k < 600; k++) req[k] = rand();
            n = rand() % 600;
        } else {
            n = rand() % 2 ? n : (size_t)(rand() % (n + 1));
            for (int k = 0; k < 3; k++) req[rand() % 600] ^= rand();
        }
        dhcp_reply_t r = dhcp_core_handle(&d, req, n, resp, &rlen, &dst, 100 + i / 100);
        assert(r == DHCP_REPLY_NONE || (rlen >= 300 && rlen <= 576));
        if (r != DHCP_REPLY_NONE) {             /* probe parser on cut / corrupted replies, exact-size buffer */
            size_t m = rand() % (rlen + 1);
            uint8_t *h = malloc(m ? m : 1);
            memcpy(h, resp, m);
            if (m && rand() % 2) h[rand() % m] ^= rand();
            dhcp_probe_is_offer(h, m, mac, rand(), d.server_ip, NULL);
            free(h);
        }
        if (i % 1000 == 0) {
            size_t s = dhcp_core_save(&d, buf, sizeof(buf), 100 + i / 100);
            static dhcp_core_t e;
            dhcp_core_init(&e, d.server_ip, d.netmask, d.pool_first, d.pool_last, d.lease_s);
            assert(dhcp_core_load(&e, buf, s, 5));
            for (int k = 0; k < 40; k++) buf[rand() % (s ? s : 1)] ^= rand();
            dhcp_core_load(&e, buf, rand() % (s + 1), 5);       /* corrupted: must not crash */
        }
    }

    /* no two live leases share an address */
    for (int i = 0; i < DHCP_MAX_LEASES; i++)
        for (int j = i + 1; j < DHCP_MAX_LEASES; j++) {
            const dhcp_lease_t *a = &d.leases[i], *b = &d.leases[j];
            if (a->used && b->used && a->ip == b->ip) {
                int32_t la = (int32_t)(a->expires - 3100), lb = (int32_t)(b->expires - 3100);
                assert(!((a->reserved || la > 0) && (b->reserved || lb > 0)));
            }
        }
    puts("all dhcp_core robustness tests passed");
    return 0;
}
