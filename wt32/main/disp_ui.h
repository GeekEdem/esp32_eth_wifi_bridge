/* Display pages for a 128x64 monochrome screen (pure C, host-testable).
 *
 * The framebuffer uses the SSD1306 layout: 8 pages of 8 rows, one byte per
 * column, bit 0 = top row. Text is UTF-8 in the 6x10 font (21 characters per
 * line); anything longer is cut with "…". A page is a title bar plus five
 * lines. Which pages exist depends on the mode (see ui_page_count()).
 *
 * Texts come from the page language files (keys "disp.*" in main/i18n/<code>.json,
 * with {placeholders}); ui_set_texts() takes one language's JSON object as
 * the firmware bundles it. Until then a text shows as its key. The font has
 * Latin and Cyrillic; other scripts show as "?".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_W        128
#define UI_H        64
#define UI_FB_SIZE  (UI_W * UI_H / 8)
#define UI_COLS     21                  /* characters per line */
#define UI_MAX_OUT  5                   /* script outputs shown */

typedef enum {
    UI_CLIENT,
    UI_ROUTER,
    UI_AP,
} ui_mode_t;

typedef enum {
    UI_UPLINK_NONE,
    UI_UPLINK_WAITING,
    UI_UPLINK_DHCP,
    UI_UPLINK_FALLBACK,
} ui_uplink_t;

/* What the pages show; addresses in network byte order, 0 = none. */
typedef struct {
    ui_mode_t mode;
    bool setup_boot;                    /* one-time setup start (button) */
    /* client */
    int wifi_state;                     /* 0 not configured, 1 connecting, 2 connected */
    char wifi_ssid[33];
    int rssi;
    bool setup_ap;
    char setup_ssid[33];
    uint32_t mgmt_ip;
    uint16_t mgmt_port;
    uint32_t to_wifi_bytes, to_eth_bytes, dropped, tx_errors, foreign;
    /* router / access point */
    char ap_ssid[33];
    int ap_clients;
    uint32_t wt32_ip;
    uint32_t gw;
    ui_uplink_t uplink;
    /* the device on the cable (client / router) */
    bool eth_up;
    int eth_speed;                      /* negotiated Mbit/s, 0 = not known */
    bool eth_full;                      /* full duplex */
    bool dev_known;
    uint8_t dev_mac[6];
    uint32_t dev_ip;
    /* system */
    char version[32];
    uint32_t heap;
    uint32_t uptime_s;
    /* script outputs */
    int n_out;
    char out_key[UI_MAX_OUT][24];
    char out_val[UI_MAX_OUT][64];
} ui_info_t;

int ui_page_count(const ui_info_t *in);
/* Draws page `index` (taken modulo the page count) into fb. */
void ui_render_page(uint8_t *fb, const ui_info_t *in, int index);
/* While the button is held: what letting go now would do. */
void ui_render_hold(uint8_t *fb, uint32_t held_ms);
/* A full-screen message: title bar and up to three lines (NULL = empty). */
void ui_render_notice(uint8_t *fb, const char *title, const char *l1, const char *l2, const char *l3);

/* Language: the "disp.*" texts of a flat JSON object {"key":"text",...}.
 * Returns how many were taken, -1 if the JSON is malformed (texts unchanged). */
int ui_set_texts(const char *json, size_t len);
/* The text for a key (the key itself if there is none). */
const char *ui_tr(const char *key);
/* The text with {name} placeholders filled: pairs of name, value (strings),
 * ended by NULL. */
void ui_trf(char *out, size_t len, const char *key, ...);

/* The Ethernet line of the device / cable page, e.g. "Ethernet: 100M full". */
void ui_eth_line(char *buf, size_t len, const ui_info_t *in);

/* Primitives (exported for the tests). */
size_t ui_utf8_next(const char *s, uint32_t *cp);       /* bytes used; 0 at the end */
size_t ui_utf8_len(const char *s);                      /* code points */
void ui_utf8_trim(char *s);                             /* drop a cut-short last character */
/* Draws at most max_chars characters at (x, y); longer text ends with "…".
 * Returns the number of characters drawn. */
int ui_text(uint8_t *fb, int x, int y, const char *s, int max_chars, bool invert);
void ui_fill(uint8_t *fb, int x, int y, int w, int h, bool on);
bool ui_pixel(const uint8_t *fb, int x, int y);
