/* See disp_ui.h. */
#include "disp_ui.h"

#include <stdio.h>
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

/* ---------- formatting ---------- */

static void fmt_ip(char *buf, size_t len, uint32_t ip)
{
    const uint8_t *b = (const uint8_t *)&ip;           /* network order in memory */
    snprintf(buf, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void fmt_bytes(char *buf, size_t len, uint32_t v)
{
    if (v < 1024) {
        snprintf(buf, len, "%lu Б", (unsigned long)v);
    } else if (v < 1024u * 1024) {
        snprintf(buf, len, "%lu.%lu КБ", (unsigned long)(v / 1024), (unsigned long)(v % 1024 * 10 / 1024));
    } else {
        uint32_t m = v / (1024u * 1024);
        snprintf(buf, len, "%lu.%lu МБ", (unsigned long)m, (unsigned long)(v % (1024u * 1024) / 104858 % 10));
    }
}

static void fmt_uptime(char *buf, size_t len, uint32_t s)
{
    uint32_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d) {
        snprintf(buf, len, "%lu д %02lu:%02lu", (unsigned long)d, (unsigned long)h, (unsigned long)m);
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
    char ip[16];
    if (in->mode == UI_CLIENT) {
        snprintf(l[0], LINE_LEN, in->setup_boot ? "Режим: налаштування" : "Режим: клієнт");
        if (in->wifi_state == 2) {
            snprintf(l[1], LINE_LEN, "Wi-Fi: %s", in->wifi_ssid);
            snprintf(l[2], LINE_LEN, "Сигнал: %d дБм", in->rssi);
        } else if (in->wifi_state == 1) {
            snprintf(l[1], LINE_LEN, "Wi-Fi: підключення…");
            snprintf(l[2], LINE_LEN, "%s", in->wifi_ssid);
        } else {
            snprintf(l[1], LINE_LEN, "Wi-Fi: не налаштовано");
        }
        if (in->mgmt_ip && !in->setup_boot) {
            fmt_ip(ip, sizeof(ip), in->mgmt_ip);
            snprintf(l[3], LINE_LEN, "Сторінка WT32:");
            snprintf(l[4], LINE_LEN, "%s:%u", ip, in->mgmt_port);
        } else if (in->setup_ap) {
            snprintf(l[3], LINE_LEN, "%s", in->setup_ssid);
            snprintf(l[4], LINE_LEN, "http://192.168.4.1");
        } else {
            snprintf(l[3], LINE_LEN, "Сторінка — коли");
            snprintf(l[4], LINE_LEN, "пристрій отримає IP");
        }
        return;
    }
    snprintf(l[0], LINE_LEN, in->mode == UI_ROUTER ? "Режим: роутер" : "Режим: точка доступу");
    snprintf(l[1], LINE_LEN, "Wi-Fi: %s", in->ap_ssid);
    snprintf(l[2], LINE_LEN, "Клієнтів Wi-Fi: %d", in->ap_clients);
    snprintf(l[3], LINE_LEN, in->uplink == UI_UPLINK_FALLBACK ? "Сторінка (запасна):" : "Сторінка WT32:");
    if (in->wt32_ip) {
        fmt_ip(ip, sizeof(ip), in->wt32_ip);
        snprintf(l[4], LINE_LEN, "http://%s", ip);
    } else {
        snprintf(l[4], LINE_LEN, "чекаю DHCP…");
    }
}

static void device_lines(const ui_info_t *in, lines_t l)
{
    snprintf(l[0], LINE_LEN, "Ethernet: %s", in->eth_up ? "є лінк" : "немає лінку");
    snprintf(l[1], LINE_LEN, "MAC пристрою:");
    if (in->dev_known) {
        const uint8_t *m = in->dev_mac;
        snprintf(l[2], LINE_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
        snprintf(l[2], LINE_LEN, "ще не бачили");
    }
    snprintf(l[3], LINE_LEN, "IP пристрою:");
    if (in->dev_ip) {
        fmt_ip(l[4], LINE_LEN, in->dev_ip);
    } else {
        snprintf(l[4], LINE_LEN, in->dev_known ? (in->mode == UI_ROUTER ? "чекаю DHCP…" : "ще невідома") : "—");
    }
}

static void cable_lines(const ui_info_t *in, lines_t l)
{
    static const char *const UPLINK[] = { "—", "чекаю…", "працює", "немає (запасна IP)" };
    snprintf(l[0], LINE_LEN, "Ethernet: %s", in->eth_up ? "є лінк" : "немає лінку");
    snprintf(l[1], LINE_LEN, "DHCP роутера:");
    snprintf(l[2], LINE_LEN, "%s", UPLINK[in->uplink <= UI_UPLINK_FALLBACK ? in->uplink : 0]);
    snprintf(l[3], LINE_LEN, "Шлюз:");
    if (in->gw) {
        fmt_ip(l[4], LINE_LEN, in->gw);
    } else {
        snprintf(l[4], LINE_LEN, "—");
    }
}

static void traffic_lines(const ui_info_t *in, lines_t l)
{
    char v[16];
    fmt_bytes(v, sizeof(v), in->to_wifi_bytes);
    snprintf(l[0], LINE_LEN, "До Wi-Fi: %s", v);
    fmt_bytes(v, sizeof(v), in->to_eth_bytes);
    snprintf(l[1], LINE_LEN, "До Ethernet: %s", v);
    snprintf(l[2], LINE_LEN, "Відкинуто: %lu", (unsigned long)in->dropped);
    snprintf(l[3], LINE_LEN, "Помилки: %lu", (unsigned long)in->tx_errors);
    snprintf(l[4], LINE_LEN, "Чужі кадри: %lu", (unsigned long)in->foreign);
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
    char up[24];
    fmt_uptime(up, sizeof(up), in->uptime_s);
    snprintf(l[0], LINE_LEN, "Версія %s", in->version);
    snprintf(l[1], LINE_LEN, "Памʼять: %lu КБ", (unsigned long)(in->heap / 1024));
    snprintf(l[2], LINE_LEN, "Працює: %s", up);
    snprintf(l[3], LINE_LEN, "Кнопка 5 с: налашт.");
    snprintf(l[4], LINE_LEN, "Кнопка 10 с: скидання");
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
    static const char *const TITLES[] = { "Мережа", "Пристрій", "Кабель", "Трафік", "Скрипт", "Система" };
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
    title(fb, in->setup_boot && p[index] == P_NETWORK ? "Налаштування" : TITLES[p[index]], index, n);
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
    if (held_ms < BTN_SETUP_MS) {
        ui_render_notice(fb, "Кнопка", "Тримайте до 5 с:", "точка налаштування", "Відпустіть: скасувати");
        bar(fb, held_ms, BTN_SETUP_MS);
    } else if (held_ms < BTN_RESET_MS) {
        ui_render_notice(fb, "Кнопка", "Відпустіть зараз:", "точка налаштування", "До 10 с: скидання");
        bar(fb, held_ms - BTN_SETUP_MS, BTN_RESET_MS - BTN_SETUP_MS);
    } else {
        ui_render_notice(fb, "Кнопка", "Відпустіть зараз:", "СКИДАННЯ налаштувань", "Wi-Fi, режим, пароль");
        ui_fill(fb, 0, TITLE_H + 1 + 2 * LINE_H, UI_W, LINE_H, false);
        ui_text(fb, 1, TITLE_H + 1 + 2 * LINE_H, "СКИДАННЯ налаштувань", UI_COLS, true);
    }
}
