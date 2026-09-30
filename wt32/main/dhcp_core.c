/* DHCP server logic. See dhcp_core.h. */
#include "dhcp_core.h"

#include <string.h>

#define OP_REQUEST      1
#define OP_REPLY        2
#define BOOTP_LEN       236
#define COOKIE_OFF      236
#define OPTIONS_OFF     240
#define MIN_REPLY       300
#define MAX_REPLY       576
#define OFFER_HOLD_S    60

#define DISCOVER 1
#define OFFER    2
#define REQUEST  3
#define DECLINE  4
#define ACK      5
#define NAK      6
#define RELEASE  7
#define INFORM   8

#define OPT_PAD         0
#define OPT_MASK        1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_HOSTNAME    12
#define OPT_REQ_IP      50
#define OPT_LEASE       51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_T1          58
#define OPT_T2          59
#define OPT_END         255

static const uint8_t COOKIE[4] = { 0x63, 0x82, 0x53, 0x63 };
static const uint8_t ZERO_MAC[6];

/* ---------- byte order without platform headers ---------- */

static uint32_t to_host(uint32_t net)
{
    const uint8_t *b = (const uint8_t *)&net;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint32_t to_net(uint32_t host)
{
    uint32_t net;
    uint8_t *b = (uint8_t *)&net;
    b[0] = host >> 24;
    b[1] = host >> 16;
    b[2] = host >> 8;
    b[3] = host;
    return net;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void put32be(uint8_t *p, uint32_t host)
{
    p[0] = host >> 24;
    p[1] = host >> 16;
    p[2] = host >> 8;
    p[3] = host;
}

/* ---------- address rules ---------- */

static bool in_subnet(const dhcp_core_t *d, uint32_t ip)
{
    uint32_t h = to_host(ip), m = to_host(d->netmask), s = to_host(d->server_ip);
    if ((h & m) != (s & m)) {
        return false;
    }
    uint32_t hostpart = h & ~m;
    return hostpart != 0 && hostpart != (~m & 0xFFFFFFFFu) && ip != d->server_ip;
}

static bool in_pool(const dhcp_core_t *d, uint32_t ip)
{
    uint32_t h = to_host(ip);
    return in_subnet(d, ip) && h >= to_host(d->pool_first) && h <= to_host(d->pool_last);
}

static bool active(const dhcp_lease_t *l, uint32_t now)
{
    return l->used && (l->reserved || (int32_t)(l->expires - now) > 0);
}

static dhcp_lease_t *by_mac(dhcp_core_t *d, const uint8_t mac[6])
{
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        if (d->leases[i].used && memcmp(d->leases[i].mac, mac, 6) == 0) {
            return &d->leases[i];
        }
    }
    return NULL;
}

/* Is ip held by someone other than mac (a live lease, an offer, a
 * reservation or a declined address)? */
static bool taken_by_other(const dhcp_core_t *d, uint32_t ip, const uint8_t mac[6], uint32_t now)
{
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        const dhcp_lease_t *l = &d->leases[i];
        if (l->used && l->ip == ip && memcmp(l->mac, mac, 6) != 0 && active(l, now)) {
            return true;
        }
    }
    return false;
}

static bool known_ip(const dhcp_core_t *d, uint32_t ip)
{
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        if (d->leases[i].used && d->leases[i].ip == ip) {
            return true;
        }
    }
    return false;
}

/* May mac use ip (for an ACK)? */
static bool allowed(const dhcp_core_t *d, const uint8_t mac[6], uint32_t ip, uint32_t now)
{
    const dhcp_lease_t *own = NULL;
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        if (d->leases[i].used && memcmp(d->leases[i].mac, mac, 6) == 0) {
            own = &d->leases[i];
        }
    }
    if (own && own->reserved) {
        return ip == own->ip;
    }
    return in_pool(d, ip) && !taken_by_other(d, ip, mac, now);
}

static uint32_t choose_ip(dhcp_core_t *d, const uint8_t mac[6], uint32_t requested, uint32_t now)
{
    dhcp_lease_t *own = by_mac(d, mac);
    if (own && own->reserved) {
        return own->ip;
    }
    if (own && in_pool(d, own->ip) && !taken_by_other(d, own->ip, mac, now)) {
        return own->ip;                 /* same address as before */
    }
    if (requested && in_pool(d, requested) && !taken_by_other(d, requested, mac, now)) {
        return requested;
    }
    /* A never-used address first, then one whose lease has run out. */
    uint32_t first = to_host(d->pool_first), last = to_host(d->pool_last);
    uint32_t fallback = 0;
    for (uint32_t h = first; h <= last && h >= first; h++) {
        uint32_t ip = to_net(h);
        if (!in_pool(d, ip) || taken_by_other(d, ip, mac, now)) {
            continue;
        }
        if (!known_ip(d, ip)) {
            return ip;
        }
        if (!fallback) {
            fallback = ip;
        }
    }
    return fallback;
}

/* The record for mac, creating one if needed (evicting the oldest expired
 * non-reserved record when full). */
static dhcp_lease_t *record_for(dhcp_core_t *d, const uint8_t mac[6], uint32_t now)
{
    dhcp_lease_t *l = by_mac(d, mac);
    if (l) {
        return l;
    }
    dhcp_lease_t *victim = NULL;
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        dhcp_lease_t *c = &d->leases[i];
        if (!c->used) {
            victim = c;
            break;
        }
        if (!active(c, now) && (!victim || (int32_t)(c->expires - victim->expires) < 0)) {
            victim = c;
        }
    }
    if (!victim) {
        return NULL;
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true;
    memcpy(victim->mac, mac, 6);
    return victim;
}

/* Another client's record holding ip without being active is stale:
 * drop its address so two records never share one. */
static void release_stale(dhcp_core_t *d, uint32_t ip, const uint8_t mac[6])
{
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        dhcp_lease_t *l = &d->leases[i];
        if (l->used && !l->reserved && l->ip == ip && memcmp(l->mac, mac, 6) != 0) {
            l->used = false;
        }
    }
}

/* ---------- messages ---------- */

typedef struct {
    uint8_t type;
    const uint8_t *mac;
    uint32_t ciaddr;
    uint32_t giaddr;
    uint32_t req_ip;            /* option 50, 0 = absent */
    uint32_t server_id;         /* option 54, 0 = absent */
    char host[DHCP_HOST_LEN];
} msg_t;

static bool parse(const uint8_t *p, size_t len, msg_t *m)
{
    if (len < OPTIONS_OFF || p[0] != OP_REQUEST || p[1] != 1 || p[2] != 6 ||
        memcmp(p + COOKIE_OFF, COOKIE, 4) != 0) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    m->mac = p + 28;
    m->ciaddr = rd32(p + 12);
    m->giaddr = rd32(p + 24);
    size_t i = OPTIONS_OFF;
    while (i < len) {
        uint8_t code = p[i];
        if (code == OPT_PAD) {
            i++;
            continue;
        }
        if (code == OPT_END || i + 1 >= len) {
            break;
        }
        uint8_t olen = p[i + 1];
        if (i + 2 + olen > len) {
            return false;
        }
        const uint8_t *v = p + i + 2;
        if (code == OPT_MSG_TYPE && olen == 1) {
            m->type = v[0];
        } else if (code == OPT_REQ_IP && olen == 4) {
            m->req_ip = rd32(v);
        } else if (code == OPT_SERVER_ID && olen == 4) {
            m->server_id = rd32(v);
        } else if (code == OPT_HOSTNAME && olen > 0) {
            size_t n = olen < DHCP_HOST_LEN - 1 ? olen : DHCP_HOST_LEN - 1;
            for (size_t k = 0; k < n; k++) {
                char c = (char)v[k];
                m->host[k] = (c >= 0x20 && c < 0x7F) ? c : '?';
            }
            m->host[n] = '\0';
        }
        i += 2 + olen;
    }
    return m->type != 0;
}

static uint8_t *opt32(uint8_t *o, uint8_t code, uint32_t net)
{
    o[0] = code;
    o[1] = 4;
    memcpy(o + 2, &net, 4);
    return o + 6;
}

static size_t build(const dhcp_core_t *d, const uint8_t *req, uint8_t type, uint32_t yiaddr,
                    uint32_t ciaddr, bool lease_opts, uint8_t *out)
{
    memset(out, 0, MAX_REPLY);
    out[0] = OP_REPLY;
    out[1] = 1;
    out[2] = 6;
    memcpy(out + 4, req + 4, 4);            /* xid */
    memcpy(out + 10, req + 10, 2);          /* flags */
    memcpy(out + 12, &ciaddr, 4);
    memcpy(out + 16, &yiaddr, 4);
    memcpy(out + 24, req + 24, 4);          /* giaddr */
    memcpy(out + 28, req + 28, 16);         /* chaddr */
    memcpy(out + COOKIE_OFF, COOKIE, 4);

    uint8_t *o = out + OPTIONS_OFF;
    *o++ = OPT_MSG_TYPE;
    *o++ = 1;
    *o++ = type;
    o = opt32(o, OPT_SERVER_ID, d->server_ip);
    if (type != NAK) {
        if (lease_opts) {
            uint8_t v[4];
            put32be(v, d->lease_s);
            *o++ = OPT_LEASE; *o++ = 4; memcpy(o, v, 4); o += 4;
            put32be(v, d->lease_s / 2);
            *o++ = OPT_T1; *o++ = 4; memcpy(o, v, 4); o += 4;
            put32be(v, d->lease_s / 8 * 7);
            *o++ = OPT_T2; *o++ = 4; memcpy(o, v, 4); o += 4;
        }
        o = opt32(o, OPT_MASK, d->netmask);
        if (d->router) {
            o = opt32(o, OPT_ROUTER, d->router);
        }
        if (d->dns) {
            o = opt32(o, OPT_DNS, d->dns);
        }
    }
    *o++ = OPT_END;
    size_t len = o - out;
    return len < MIN_REPLY ? MIN_REPLY : len;
}

static dhcp_reply_t send_to(const msg_t *m, uint8_t type, uint32_t *dst)
{
    if (type != NAK && m->ciaddr) {
        *dst = m->ciaddr;
        return DHCP_REPLY_UNICAST;
    }
    *dst = 0xFFFFFFFFu;
    return DHCP_REPLY_BROADCAST;
}

static dhcp_reply_t ack(dhcp_core_t *d, const uint8_t *req, const msg_t *m, uint32_t ip,
                        uint8_t *resp, size_t *resp_len, uint32_t *dst, uint32_t now)
{
    dhcp_lease_t *l = record_for(d, m->mac, now);
    if (!l) {
        return DHCP_REPLY_NONE;             /* table full of live leases */
    }
    release_stale(d, ip, m->mac);
    if (!l->reserved) {
        l->ip = ip;
        l->expires = now + d->lease_s;
    }
    l->bound = true;
    if (m->host[0]) {
        memcpy(l->host, m->host, DHCP_HOST_LEN);
    }
    d->dirty = true;
    *resp_len = build(d, req, ACK, ip, m->ciaddr, true, resp);
    return send_to(m, ACK, dst);
}

static dhcp_reply_t nak(const dhcp_core_t *d, const uint8_t *req, const msg_t *m,
                        uint8_t *resp, size_t *resp_len, uint32_t *dst)
{
    *resp_len = build(d, req, NAK, 0, 0, false, resp);
    return send_to(m, NAK, dst);
}

void dhcp_core_init(dhcp_core_t *d, uint32_t server_ip, uint32_t netmask,
                    uint32_t pool_first, uint32_t pool_last, uint32_t lease_s)
{
    memset(d, 0, sizeof(*d));
    d->server_ip = server_ip;
    d->netmask = netmask;
    d->pool_first = pool_first;
    d->pool_last = pool_last;
    d->lease_s = lease_s;
}

dhcp_reply_t dhcp_core_handle(dhcp_core_t *d, const uint8_t *req, size_t len,
                              uint8_t *resp, size_t *resp_len, uint32_t *dst, uint32_t now)
{
    msg_t m;
    if (!parse(req, len, &m) || m.giaddr != 0 || memcmp(m.mac, ZERO_MAC, 6) == 0) {
        return DHCP_REPLY_NONE;
    }

    switch (m.type) {
    case DISCOVER: {
        uint32_t ip = choose_ip(d, m.mac, m.req_ip, now);
        if (!ip) {
            return DHCP_REPLY_NONE;         /* pool exhausted */
        }
        dhcp_lease_t *l = record_for(d, m.mac, now);
        if (!l) {
            return DHCP_REPLY_NONE;
        }
        if (!l->reserved) {
            release_stale(d, ip, m.mac);
            if (!(l->bound && l->ip == ip && active(l, now))) {
                l->ip = ip;
                l->bound = false;
                l->expires = now + OFFER_HOLD_S;   /* hold it for this client */
            }
        }
        if (m.host[0]) {
            memcpy(l->host, m.host, DHCP_HOST_LEN);
        }
        *resp_len = build(d, req, OFFER, ip, 0, true, resp);
        return send_to(&m, OFFER, dst);
    }

    case REQUEST:
        if (m.server_id) {                  /* SELECTING */
            if (m.server_id != d->server_ip) {
                dhcp_lease_t *l = by_mac(d, m.mac);
                if (l && !l->bound && !l->reserved) {
                    l->used = false;        /* it took another server's offer */
                }
                return DHCP_REPLY_NONE;
            }
            if (!m.req_ip) {
                return DHCP_REPLY_NONE;
            }
            return allowed(d, m.mac, m.req_ip, now) ? ack(d, req, &m, m.req_ip, resp, resp_len, dst, now)
                                                    : nak(d, req, &m, resp, resp_len, dst);
        }
        if (m.req_ip) {                     /* INIT-REBOOT */
            return allowed(d, m.mac, m.req_ip, now) ? ack(d, req, &m, m.req_ip, resp, resp_len, dst, now)
                                                    : nak(d, req, &m, resp, resp_len, dst);
        }
        if (m.ciaddr) {                     /* RENEWING / REBINDING */
            return allowed(d, m.mac, m.ciaddr, now) ? ack(d, req, &m, m.ciaddr, resp, resp_len, dst, now)
                                                    : nak(d, req, &m, resp, resp_len, dst);
        }
        return DHCP_REPLY_NONE;

    case RELEASE: {
        dhcp_lease_t *l = by_mac(d, m.mac);
        if (l && !l->reserved && l->ip == m.ciaddr) {
            l->expires = now;               /* keep the record: same address next time */
            l->bound = false;
            d->dirty = true;
        }
        return DHCP_REPLY_NONE;
    }

    case DECLINE: {
        /* The client found the address in use: park it for a while. */
        dhcp_lease_t *l = by_mac(d, m.mac);
        if (l && !l->reserved && m.req_ip && l->ip == m.req_ip) {
            memset(l->mac, 0, 6);
            l->host[0] = '\0';
            l->bound = true;
            l->expires = now + DHCP_DECLINED_S;
            d->dirty = true;
        }
        return DHCP_REPLY_NONE;
    }

    case INFORM:
        if (!m.ciaddr) {
            return DHCP_REPLY_NONE;
        }
        *resp_len = build(d, req, ACK, 0, m.ciaddr, false, resp);
        *dst = m.ciaddr;
        return DHCP_REPLY_UNICAST;

    default:
        return DHCP_REPLY_NONE;
    }
}

bool dhcp_core_reserve(dhcp_core_t *d, const uint8_t mac[6], uint32_t ip)
{
    if (!in_subnet(d, ip) || memcmp(mac, ZERO_MAC, 6) == 0 || (mac[0] & 1)) {
        return false;
    }
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        const dhcp_lease_t *l = &d->leases[i];
        if (l->used && l->reserved && l->ip == ip && memcmp(l->mac, mac, 6) != 0) {
            return false;                   /* reserved for someone else */
        }
    }
    dhcp_lease_t *l = by_mac(d, mac);
    if (!l) {
        /* Make room even among live leases: a reservation outranks them. */
        l = record_for(d, mac, 0xFFFFFFFFu);
        if (!l) {
            return false;
        }
    }
    l->reserved = true;
    l->ip = ip;
    l->expires = 0;
    d->dirty = true;
    return true;
}

bool dhcp_core_unreserve(dhcp_core_t *d, const uint8_t mac[6])
{
    dhcp_lease_t *l = by_mac(d, mac);
    if (!l || !l->reserved) {
        return false;
    }
    l->reserved = false;
    l->bound = false;
    l->expires = 0;                         /* free now; affinity kept if still in the pool */
    d->dirty = true;
    return true;
}

uint32_t dhcp_core_ip_of(const dhcp_core_t *d, const uint8_t mac[6], uint32_t now)
{
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        const dhcp_lease_t *l = &d->leases[i];
        if (l->used && memcmp(l->mac, mac, 6) == 0 && (l->reserved || (l->bound && active(l, now)))) {
            return l->ip;
        }
    }
    return 0;
}

/* ---------- persistence ---------- */

#define SAVE_MAGIC  0x31434844u     /* "DHC1" */
#define ENTRY_LEN   (6 + 4 + 1 + 4 + DHCP_HOST_LEN)

size_t dhcp_core_save(const dhcp_core_t *d, uint8_t *buf, size_t cap, uint32_t now)
{
    if (cap < 5) {
        return 0;
    }
    put32be(buf, SAVE_MAGIC);
    size_t pos = 5;
    uint8_t count = 0;
    for (int i = 0; i < DHCP_MAX_LEASES && pos + ENTRY_LEN <= cap; i++) {
        const dhcp_lease_t *l = &d->leases[i];
        if (!l->used || memcmp(l->mac, ZERO_MAC, 6) == 0 || !(l->reserved || (l->bound && active(l, now)))) {
            continue;                       /* only what a client may still hold */
        }
        memcpy(buf + pos, l->mac, 6);
        memcpy(buf + pos + 6, &l->ip, 4);
        buf[pos + 10] = l->reserved ? 1 : 0;
        put32be(buf + pos + 11, l->reserved ? 0 : l->expires - now);
        memcpy(buf + pos + 15, l->host, DHCP_HOST_LEN);
        buf[pos + 15 + DHCP_HOST_LEN - 1] = '\0';
        pos += ENTRY_LEN;
        count++;
    }
    buf[4] = count;
    return pos;
}

bool dhcp_core_load(dhcp_core_t *d, const uint8_t *buf, size_t len, uint32_t now)
{
    if (len < 5 || to_host(rd32(buf)) != SAVE_MAGIC || len < 5 + (size_t)buf[4] * ENTRY_LEN) {
        return false;
    }
    int slot = 0;
    for (int i = 0; i < buf[4] && slot < DHCP_MAX_LEASES; i++) {
        const uint8_t *e = buf + 5 + i * ENTRY_LEN;
        uint32_t ip = rd32(e + 6);
        bool reserved = e[10] & 1;
        uint32_t remaining = to_host(rd32(e + 11));
        if (!in_subnet(d, ip) || (!reserved && (remaining == 0 || !in_pool(d, ip)))) {
            continue;                       /* the network changed since */
        }
        dhcp_lease_t *l = &d->leases[slot++];
        memset(l, 0, sizeof(*l));
        l->used = true;
        l->reserved = reserved;
        l->bound = true;
        memcpy(l->mac, e, 6);
        l->ip = ip;
        l->expires = reserved ? 0 : now + remaining;
        memcpy(l->host, e + 15, DHCP_HOST_LEN);
        l->host[DHCP_HOST_LEN - 1] = '\0';
    }
    d->dirty = false;
    return true;
}

/* ---------- probe for another server (access-point mode) ---------- */

size_t dhcp_probe_build(uint8_t *buf, size_t cap, const uint8_t mac[6], uint32_t xid)
{
    if (cap < MIN_REPLY) {
        return 0;
    }
    memset(buf, 0, MIN_REPLY);
    buf[0] = OP_REQUEST;
    buf[1] = 1;
    buf[2] = 6;
    put32be(buf + 4, xid);
    buf[10] = 0x80;                         /* broadcast flag: we have no usable address */
    memcpy(buf + 28, mac, 6);
    memcpy(buf + COOKIE_OFF, COOKIE, 4);
    uint8_t *o = buf + OPTIONS_OFF;
    *o++ = OPT_MSG_TYPE;
    *o++ = 1;
    *o++ = DISCOVER;
    *o++ = 55;                              /* parameter request list */
    *o++ = 2;
    *o++ = OPT_MASK;
    *o++ = OPT_ROUTER;
    *o++ = OPT_END;
    return MIN_REPLY;
}

bool dhcp_probe_is_offer(const uint8_t *p, size_t len, const uint8_t mac[6], uint32_t xid,
                         uint32_t own_ip, uint32_t *server_id)
{
    uint8_t x[4];
    put32be(x, xid);
    if (len < OPTIONS_OFF || p[0] != OP_REPLY || p[1] != 1 || p[2] != 6 ||
        memcmp(p + 4, x, 4) != 0 || memcmp(p + 28, mac, 6) != 0 ||
        memcmp(p + COOKIE_OFF, COOKIE, 4) != 0) {
        return false;
    }
    uint8_t type = 0;
    uint32_t sid = 0;
    size_t i = OPTIONS_OFF;
    while (i < len) {
        uint8_t code = p[i];
        if (code == OPT_PAD) {
            i++;
            continue;
        }
        if (code == OPT_END || i + 1 >= len) {
            break;
        }
        uint8_t olen = p[i + 1];
        if (i + 2 + olen > len) {
            return false;
        }
        if (code == OPT_MSG_TYPE && olen == 1) {
            type = p[i + 2];
        } else if (code == OPT_SERVER_ID && olen == 4) {
            sid = rd32(p + i + 2);
        }
        i += 2 + olen;
    }
    /* our own fallback server never answers the probe, but be sure */
    if (type != OFFER || sid == own_ip || rd32(p + 16) == 0) {
        return false;
    }
    if (server_id) {
        *server_id = sid;
    }
    return true;
}
