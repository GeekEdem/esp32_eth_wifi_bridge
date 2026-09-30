/* See disp_ui.h. */
#include "disp_ui.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "button.h"
#include "font6x10.h"

#define LINE_H      10
#define TITLE_H     10
#define LINES       5
#define LINE_LEN    96                  /* >= 21 characters of UTF-8, and key + value of an output */

/* ---------- primitives ---------- */

bool ui_pixel(const uint8_t *fb, int x, int y)
{
    return x >= 0 && x < UI_W && y >= 0 && y < UI_H && (fb[(y / 8) * UI_W + x] >> (y & 7) & 1);
}

static void pixel(uint8_t *fb, int x, int y, bool on)
{
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) {
        return;
    }
    uint8_t *p = &fb[(y / 8) * UI_W + x];
    if (on) {
        *p |= 1 << (y & 7);
    } else {
        *p &= ~(1 << (y & 7));
    }
}

void ui_fill(uint8_t *fb, int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            pixel(fb, i, j, on);
        }
    }
}

size_t ui_utf8_next(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (!p[0]) {
        return 0;
    }
    int n = p[0] < 0x80 ? 1 : (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0) {
        *cp = '?';                      /* stray continuation or invalid lead byte */
        return 1;
    }
    uint32_t v = n == 1 ? p[0] : p[0] & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {    /* also stops at the terminating NUL */
            *cp = '?';
            return i;
        }
        v = v << 6 | (p[i] & 0x3F);
    }
    *cp = v;
    return n;
}

void ui_utf8_trim(char *s)
{
    size_t n = strlen(s), i = n;
    while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) {
        i--;                            /* back over continuation bytes */
    }
    if (i == 0) {
        return;
    }
    uint8_t lead = (uint8_t)s[i - 1];
    size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
    if (n - (i - 1) < need) {
        s[i - 1] = '\0';                /* the last sequence was cut short */
    }
}

size_t ui_utf8_len(const char *s)
{
    size_t n = 0, k;
    uint32_t cp;
    while ((k = ui_utf8_next(s, &cp)) != 0) {
        s += k;
        n++;
    }
    return n;
}

static const uint8_t *glyph(uint32_t cp)
{
    size_t lo = 0, hi = font6x10_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (font6x10[mid].cp == cp) {
            return font6x10[mid].rows;
        }
        if (font6x10[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return cp == '?' ? NULL : glyph('?');
}

static void draw_char(uint8_t *fb, int x, int y, uint32_t cp, bool invert)
{
    const uint8_t *rows = glyph(cp);
    for (int j = 0; j < FONT_H; j++) {
        for (int i = 0; i < FONT_W; i++) {
            bool on = rows && (rows[j] & (0x80 >> i));
            pixel(fb, x + i, y + j, on != invert);
        }
    }
}

int ui_text(uint8_t *fb, int x, int y, const char *s, int max_chars, bool invert)
{
    if (max_chars <= 0) {
        return 0;
    }
    bool cut = ui_utf8_len(s) > (size_t)max_chars;
    int limit = cut ? max_chars - 1 : max_chars;
    int n = 0;
    size_t k;
    uint32_t cp;
    while (n < limit && (k = ui_utf8_next(s, &cp)) != 0) {
        draw_char(fb, x + n * FONT_W, y, cp, invert);
        s += k;
        n++;
    }
    if (cut) {
        draw_char(fb, x + n * FONT_W, y, 0x2026, invert);    /* … */
        n++;
    }
    return n;
}

/* ---------- texts ---------- */

#define TEXTS_MAX       96
#define TEXTS_ARENA     3072

static char s_arena[TEXTS_ARENA];
static struct {
    const char *key, *val;
} s_texts[TEXTS_MAX];
static int s_ntexts;

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* A JSON string at p (on the opening quote), unescaped into out (NUL added;
 * out NULL: only skipped). Returns the position after the closing quote,
 * NULL if malformed or too long. */
static const char *json_string(const char *p, const char *end, char *out, size_t cap)
{
    if (p >= end || *p != '"') {
        return NULL;
    }
    size_t n = 0;
    for (p++; p < end && *p != '"'; p++) {
        char enc[3];
        size_t k = 0;
        if (*p != '\\') {
            enc[k++] = *p;                          /* raw UTF-8 bytes pass through */
        } else if (++p >= end) {
            return NULL;
        } else if (*p == 'u') {
            int v = end - p > 4 ? hex4(p + 1) : -1;
            if (v < 0) {
                return NULL;
            }
            p += 4;
            if (v >= 0xD800 && v <= 0xDFFF) {
                v = '?';                            /* no surrogate pairs (not in the font anyway) */
            }
            if (v < 0x80) {
                enc[k++] = (char)v;
            } else if (v < 0x800) {
                enc[k++] = (char)(0xC0 | v >> 6);
                enc[k++] = (char)(0x80 | (v & 0x3F));
            } else {
                enc[k++] = (char)(0xE0 | v >> 12);
                enc[k++] = (char)(0x80 | (v >> 6 & 0x3F));
                enc[k++] = (char)(0x80 | (v & 0x3F));
            }
        } else {
            static const char from[] = "ntrbf", to[] = "\n\t\r\b\f";
            const char *e = strchr(from, *p);
            enc[k++] = e ? to[e - from] : *p;       /* \" \\ \/ */
        }
        if (out) {
            if (n + k >= cap) {
                return NULL;
            }
            memcpy(out + n, enc, k);
            n += k;
        }
    }
    if (p >= end) {
        return NULL;
    }
    if (out) {
        out[n] = '\0';
    }
    return p + 1;
}

/* Walks the object; with store, keeps the "disp.*" texts. Returns their
 * count, -1 if malformed. */
static int parse_texts(const char *json, size_t len, bool store)
{
    int n = 0;
    size_t used = 0;
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p++ != '{') {
        return -1;
    }
    p = skip_ws(p, end);
    if (p < end && *p == '}') {
        return 0;
    }
    while (true) {
        char key[64], val[128] = "";
        p = json_string(skip_ws(p, end), end, key, sizeof(key));
        p = p ? skip_ws(p, end) : NULL;
        if (!p || p >= end || *p++ != ':') {
            return -1;
        }
        bool mine = strncmp(key, "disp.", 5) == 0;     /* other values may be long: skipped */
        p = json_string(skip_ws(p, end), end, mine ? val : NULL, sizeof(val));
        if (!p) {
            return -1;
        }
        size_t kl = strlen(key) + 1, vl = mine ? strlen(val) + 1 : 0;
        if (mine && n < TEXTS_MAX && used + kl + vl <= sizeof(s_arena)) {
            if (store) {
                s_texts[n].key = memcpy(s_arena + used, key, kl);
                s_texts[n].val = memcpy(s_arena + used + kl, val, vl);
            }
            used += kl + vl;
            n++;
        }
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
        } else if (p < end && *p == '}') {
            return n;
        } else {
            return -1;
        }
    }
}

int ui_set_texts(const char *json, size_t len)
{
    if (parse_texts(json, len, false) < 0) {
        return -1;                                  /* keep the current texts */
    }
    s_ntexts = 0;
    int n = parse_texts(json, len, true);
    s_ntexts = n;
    return n;
}

const char *ui_tr(const char *key)
{
    for (int i = 0; i < s_ntexts; i++) {
        if (strcmp(s_texts[i].key, key) == 0) {
            return s_texts[i].val;
        }
    }
    return key;
}

void ui_trf(char *out, size_t len, const char *key, ...)
{
    const char *t = ui_tr(key);
    size_t n = 0;
    while (*t && n + 1 < len) {
        const char *close = *t == '{' ? strchr(t, '}') : NULL;
        const char *value = NULL;
        if (close) {
            va_list ap;
            va_start(ap, key);
            for (const char *name; (name = va_arg(ap, const char *)) != NULL;) {
                const char *v = va_arg(ap, const char *);
                if (strlen(name) == (size_t)(close - t - 1) && strncmp(name, t + 1, close - t - 1) == 0) {
                    value = v;
                    break;
                }
            }
            va_end(ap);
        }
        if (value) {
            while (*value && n + 1 < len) {
                out[n++] = *value++;
            }
            t = close + 1;
        } else {
            out[n++] = *t++;
        }
    }
    out[n] = '\0';
    ui_utf8_trim(out);                              /* do not end mid-character */
}

/* ---------- formatting ---------- */

static void fmt_ip(char *buf, size_t len, uint32_t ip)
{
    const uint8_t *b = (const uint8_t *)&ip;           /* network order in memory */
    snprintf(buf, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void fmt_bytes(char *buf, size_t len, uint32_t v)
{
    char n[16];
    if (v < 1024) {
        snprintf(n, sizeof(n), "%lu", (unsigned long)v);
        ui_trf(buf, len, "disp.bytes", "n", n, NULL);
    } else if (v < 1024u * 1024) {
        snprintf(n, sizeof(n), "%lu.%lu", (unsigned long)(v / 1024), (unsigned long)(v % 1024 * 10 / 1024));
        ui_trf(buf, len, "disp.kb", "n", n, NULL);
    } else {
        uint32_t m = v / (1024u * 1024);
        snprintf(n, sizeof(n), "%lu.%lu", (unsigned long)m, (unsigned long)(v % (1024u * 1024) / 104858 % 10));
        ui_trf(buf, len, "disp.mb", "n", n, NULL);
    }
}

/* A number as text, for ui_trf(). */
static const char *num(char buf[12], long v)
{
    snprintf(buf, 12, "%ld", v);
    return buf;
}

static void fmt_uptime(char *buf, size_t len, uint32_t s)
{
    uint32_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d) {
        char dn[12], hm[8];
        snprintf(hm, sizeof(hm), "%02lu:%02lu", (unsigned long)h, (unsigned long)m);
        ui_trf(buf, len, "disp.days", "d", num(dn, (long)d), "hm", hm, NULL);
    } else {
        snprintf(buf, len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)(s % 60));
    }
}

/* ---------- pages ---------- */

typedef enum {
    P_NETWORK,
    P_DEVICE,
    P_CABLE,
    P_TRAFFIC,
    P_SCRIPT,
    P_SYSTEM,
} page_t;

static int pages(const ui_info_t *in, page_t out[6])
{
    int n = 0;
    out[n++] = P_NETWORK;
    out[n++] = in->mode == UI_AP ? P_CABLE : P_DEVICE;
    if (in->mode == UI_CLIENT) {
        out[n++] = P_TRAFFIC;
    }
    if (in->n_out > 0) {
        out[n++] = P_SCRIPT;
    }
    out[n++] = P_SYSTEM;
    return n;
}

int ui_page_count(const ui_info_t *in)
{
    page_t p[6];
    return pages(in, p);
}

typedef char lines_t[LINES][LINE_LEN];

static void network_lines(const ui_info_t *in, lines_t l)
{
    char ip[16], n[12];
    if (in->mode == UI_CLIENT) {
        snprintf(l[0], LINE_LEN, "%s", ui_tr(in->setup_boot ? "disp.modeSetup" : "disp.modeClient"));
        if (in->wifi_state == 2) {
            snprintf(l[1], LINE_LEN, "Wi-Fi: %s", in->wifi_ssid);
            ui_trf(l[2], LINE_LEN, "disp.signal", "n", num(n, in->rssi), NULL);
        } else if (in->wifi_state == 1) {
            snprintf(l[1], LINE_LEN, "%s", ui_tr("disp.wifiConnecting"));
            snprintf(l[2], LINE_LEN, "%s", in->wifi_ssid);
        } else {
            snprintf(l[1], LINE_LEN, "%s", ui_tr("disp.wifiNone"));
        }
        if (in->mgmt_ip && !in->setup_boot) {
            fmt_ip(ip, sizeof(ip), in->mgmt_ip);
            snprintf(l[3], LINE_LEN, "%s", ui_tr("disp.page"));
            snprintf(l[4], LINE_LEN, "%s:%u", ip, in->mgmt_port);
        } else if (in->setup_ap) {
            snprintf(l[3], LINE_LEN, "%s", in->setup_ssid);
            snprintf(l[4], LINE_LEN, "http://192.168.4.1");
        } else {
            snprintf(l[3], LINE_LEN, "%s", ui_tr("disp.pageWhen1"));
            snprintf(l[4], LINE_LEN, "%s", ui_tr("disp.pageWhen2"));
        }
        return;
    }
    snprintf(l[0], LINE_LEN, "%s", ui_tr(in->mode == UI_ROUTER ? "disp.modeRouter" : "disp.modeAp"));
    snprintf(l[1], LINE_LEN, "Wi-Fi: %s", in->ap_ssid);
    ui_trf(l[2], LINE_LEN, "disp.apClients", "n", num(n, in->ap_clients), NULL);
    snprintf(l[3], LINE_LEN, "%s", ui_tr(in->uplink == UI_UPLINK_FALLBACK ? "disp.pageFallback" : "disp.page"));
    if (in->wt32_ip) {
        fmt_ip(ip, sizeof(ip), in->wt32_ip);
        snprintf(l[4], LINE_LEN, "http://%s", ip);
    } else {
        snprintf(l[4], LINE_LEN, "%s", ui_tr("disp.waitDhcp"));
    }
}

void ui_eth_line(char *buf, size_t len, const ui_info_t *in)
{
    char n[12];
    if (!in->eth_up) {
        snprintf(buf, len, "%s", ui_tr("disp.ethNone"));
    } else if (in->eth_speed) {
        ui_trf(buf, len, in->eth_full ? "disp.ethFull" : "disp.ethHalf", "speed", num(n, in->eth_speed), NULL);
    } else {
        snprintf(buf, len, "%s", ui_tr("disp.ethUp"));
    }
}

static void device_lines(const ui_info_t *in, lines_t l)
{
    ui_eth_line(l[0], LINE_LEN, in);
    snprintf(l[1], LINE_LEN, "%s", ui_tr("disp.devMac"));
    if (in->dev_known) {
        const uint8_t *m = in->dev_mac;
        snprintf(l[2], LINE_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
        snprintf(l[2], LINE_LEN, "%s", ui_tr("disp.notSeen"));
    }
    snprintf(l[3], LINE_LEN, "%s", ui_tr("disp.devIp"));
    if (in->dev_ip) {
        fmt_ip(l[4], LINE_LEN, in->dev_ip);
    } else {
        snprintf(l[4], LINE_LEN, "%s", in->dev_known ? ui_tr(in->mode == UI_ROUTER ? "disp.waitDhcp" : "disp.ipUnknown") : "—");
    }
}

static void cable_lines(const ui_info_t *in, lines_t l)
{
    static const char *const UPLINK[] = { NULL, "disp.uplinkWaiting", "disp.uplinkDhcp", "disp.uplinkFallback" };
    const char *up = UPLINK[in->uplink <= UI_UPLINK_FALLBACK ? in->uplink : 0];
    ui_eth_line(l[0], LINE_LEN, in);
    snprintf(l[1], LINE_LEN, "%s", ui_tr("disp.routerDhcp"));
    snprintf(l[2], LINE_LEN, "%s", up ? ui_tr(up) : "—");
    snprintf(l[3], LINE_LEN, "%s", ui_tr("disp.gateway"));
    if (in->gw) {
        fmt_ip(l[4], LINE_LEN, in->gw);
    } else {
        snprintf(l[4], LINE_LEN, "—");
    }
}

static void traffic_lines(const ui_info_t *in, lines_t l)
{
    char v[24], n[12];
    fmt_bytes(v, sizeof(v), in->to_wifi_bytes);
    ui_trf(l[0], LINE_LEN, "disp.toWifi", "v", v, NULL);
    fmt_bytes(v, sizeof(v), in->to_eth_bytes);
    ui_trf(l[1], LINE_LEN, "disp.toEth", "v", v, NULL);
    snprintf(n, sizeof(n), "%lu", (unsigned long)in->dropped);
    ui_trf(l[2], LINE_LEN, "disp.dropped", "n", n, NULL);
    snprintf(n, sizeof(n), "%lu", (unsigned long)in->tx_errors);
    ui_trf(l[3], LINE_LEN, "disp.errors", "n", n, NULL);
    snprintf(n, sizeof(n), "%lu", (unsigned long)in->foreign);
    ui_trf(l[4], LINE_LEN, "disp.foreign", "n", n, NULL);
}

static void script_lines(const ui_info_t *in, lines_t l)
{
    for (int i = 0; i < in->n_out && i < UI_MAX_OUT && i < LINES; i++) {
        char key[sizeof(in->out_key[0])], val[sizeof(in->out_val[0])];
        snprintf(key, sizeof(key), "%s", in->out_key[i]);   /* may be cut mid-character */
        snprintf(val, sizeof(val), "%s", in->out_val[i]);
        ui_utf8_trim(key);
        ui_utf8_trim(val);
        snprintf(l[i], LINE_LEN, "%s: %s", key, val);
    }
}

static void system_lines(const ui_info_t *in, lines_t l)
{
    char up[32], n[12];
    fmt_uptime(up, sizeof(up), in->uptime_s);
    ui_trf(l[0], LINE_LEN, "disp.version", "v", in->version, NULL);
    snprintf(n, sizeof(n), "%lu", (unsigned long)(in->heap / 1024));
    ui_trf(l[1], LINE_LEN, "disp.memory", "n", n, NULL);
    ui_trf(l[2], LINE_LEN, "disp.uptime", "t", up, NULL);
    snprintf(l[3], LINE_LEN, "%s", ui_tr("disp.btn5"));
    snprintf(l[4], LINE_LEN, "%s", ui_tr("disp.btn10"));
}

static void title(uint8_t *fb, const char *text, int index, int count)
{
    ui_fill(fb, 0, 0, UI_W, TITLE_H, true);
    char num[24] = "";
    if (count > 0) {
        snprintf(num, sizeof(num), "%d/%d", index + 1, count);
    }
    int nlen = (int)strlen(num);
    ui_text(fb, 1, 0, text, UI_COLS - nlen - 1, true);
    ui_text(fb, UI_W - 1 - nlen * FONT_W, 0, num, nlen, true);
}

static void body(uint8_t *fb, lines_t l)
{
    for (int i = 0; i < LINES; i++) {
        ui_text(fb, 1, TITLE_H + 1 + i * LINE_H, l[i], UI_COLS, false);
    }
}

void ui_render_page(uint8_t *fb, const ui_info_t *in, int index)
{
    static const char *const TITLES[] = { "disp.tNetwork", "disp.tDevice", "disp.tCable", "disp.tTraffic",
                                          "disp.tScript", "disp.tSystem" };
    page_t p[6];
    int n = pages(in, p);
    index = ((index % n) + n) % n;
    lines_t l;
    memset(l, 0, sizeof(l));
    switch (p[index]) {
    case P_NETWORK: network_lines(in, l); break;
    case P_DEVICE:  device_lines(in, l); break;
    case P_CABLE:   cable_lines(in, l); break;
    case P_TRAFFIC: traffic_lines(in, l); break;
    case P_SCRIPT:  script_lines(in, l); break;
    case P_SYSTEM:  system_lines(in, l); break;
    }
    memset(fb, 0, UI_FB_SIZE);
    title(fb, ui_tr(in->setup_boot && p[index] == P_NETWORK ? "disp.tSetup" : TITLES[p[index]]), index, n);
    body(fb, l);
}

void ui_render_notice(uint8_t *fb, const char *t, const char *l1, const char *l2, const char *l3)
{
    lines_t l;
    memset(l, 0, sizeof(l));
    const char *src[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++) {
        snprintf(l[i + 1], LINE_LEN, "%s", src[i] ? src[i] : "");
    }
    memset(fb, 0, UI_FB_SIZE);
    title(fb, t, 0, 0);
    body(fb, l);
}

/* Progress bar under the text: how far into the current step the hold is. */
static void bar(uint8_t *fb, uint32_t done, uint32_t total)
{
    int y = UI_H - 8, w = UI_W - 4;
    ui_fill(fb, 2, y, w, 1, true);
    ui_fill(fb, 2, y + 6, w, 1, true);
    ui_fill(fb, 2, y, 1, 7, true);
    ui_fill(fb, 2 + w - 1, y, 1, 7, true);
    int fill = total ? (int)((uint64_t)(done > total ? total : done) * (w - 4) / total) : 0;
    ui_fill(fb, 4, y + 2, fill, 3, true);
}

void ui_render_hold(uint8_t *fb, uint32_t held_ms)
{
    const char *t = ui_tr("disp.tButton");
    if (held_ms < BTN_SETUP_MS) {
        ui_render_notice(fb, t, ui_tr("disp.hold"), ui_tr("disp.setupAp"), ui_tr("disp.cancel"));
        bar(fb, held_ms, BTN_SETUP_MS);
    } else if (held_ms < BTN_RESET_MS) {
        ui_render_notice(fb, t, ui_tr("disp.releaseNow"), ui_tr("disp.setupAp"), ui_tr("disp.until10"));
        bar(fb, held_ms - BTN_SETUP_MS, BTN_RESET_MS - BTN_SETUP_MS);
    } else {
        ui_render_notice(fb, t, ui_tr("disp.releaseNow"), ui_tr("disp.resetAll"), ui_tr("disp.resetWhat"));
        ui_fill(fb, 0, TITLE_H + 1 + 2 * LINE_H, UI_W, LINE_H, false);
        ui_text(fb, 1, TITLE_H + 1 + 2 * LINE_H, ui_tr("disp.resetAll"), UI_COLS, true);
    }
}
