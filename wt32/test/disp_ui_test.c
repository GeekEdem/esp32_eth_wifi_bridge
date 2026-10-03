/* Host test of disp_ui.c: UTF-8, cutting long text, every page of every mode
 * (ASan catches overruns). With a directory argument it also writes each
 * screen as a PBM image there, for a look:
 *   ./disp_ui_test /tmp/screens
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "disp_ui.h"
#include "font6x10.h"

static uint8_t fb[UI_FB_SIZE];
static const char *outdir;
static int shots;
static char prefix[8];            /* language of the screens being saved */

static void save(const char *name)
{
    if (!outdir) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%02d_%s%s.pbm", outdir, shots++, prefix, name);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "P1\n%d %d\n", UI_W, UI_H);
    for (int y = 0; y < UI_H; y++) {
        for (int x = 0; x < UI_W; x++) fputs(ui_pixel(fb, x, y) ? "1 " : "0 ", f);
        fputc('\n', f);
    }
    fclose(f);
}

static int lit(int x0, int y0, int w, int h)
{
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) n += ui_pixel(fb, x, y);
    return n;
}

static uint32_t ip(int a, int b, int c, int d)
{
    uint32_t v; uint8_t *p = (uint8_t *)&v;
    p[0] = a; p[1] = b; p[2] = c; p[3] = d;
    return v;
}

/* Two-colour modules: the title stays in the top strip and nothing crosses
 * the colour boundary (the last two rows above it stay dark). */
static void strips(void)
{
    assert(lit(0, 0, UI_W, UI_TOP_H - 2) > 900);    /* title bar is lit */
    assert(lit(0, UI_TOP_H - 2, UI_W, 2) == 0);
    assert(lit(0, UI_TOP_H, UI_W, UI_H - UI_TOP_H) > 0);   /* some text below */
}

static void all_pages(const ui_info_t *in, const char *name)
{
    int n = ui_page_count(in);
    for (int i = -1; i <= n; i++) {                 /* out-of-range indexes wrap */
        ui_render_page(fb, in, i);
        strips();
        if (i >= 0 && i < n) {
            char nm[64];
            snprintf(nm, sizeof(nm), "%s_%d", name, i + 1);
            save(nm);
        }
    }
}

/* Every page of every mode in one language (the main/i18n/<lang>.json the
 * firmware is built from); eth: the expected Ethernet lines. */
static void pages_in(const char *lang, const char *const eth[4])
{
    char path[64];
    snprintf(path, sizeof(path), "../main/i18n/%s.json", lang);
    FILE *f = fopen(path, "rb");
    assert(f && "run from wt32/test");
    static char json[65536];
    size_t len = fread(json, 1, sizeof(json), f);
    fclose(f);
    assert(len > 0 && len < sizeof(json));
    assert(ui_set_texts(json, len) > 50);
    snprintf(prefix, sizeof(prefix), "%s_", lang);

    ui_info_t in;
    memset(&in, 0, sizeof(in));

    /* the Ethernet line: negotiated speed and duplex, within one line */
    {
        char b[96];
        ui_eth_line(b, sizeof(b), &in);
        assert(!strcmp(b, eth[0]));
        in.eth_up = true;
        ui_eth_line(b, sizeof(b), &in);
        assert(!strcmp(b, eth[1]));                      /* up, details not read yet */
        in.eth_speed = 100; in.eth_full = true;
        ui_eth_line(b, sizeof(b), &in);
        assert(!strcmp(b, eth[2]) && ui_utf8_len(b) <= UI_COLS);
        in.eth_speed = 10; in.eth_full = false;
        ui_eth_line(b, sizeof(b), &in);
        assert(!strcmp(b, eth[3]) && ui_utf8_len(b) <= UI_COLS);
        memset(&in, 0, sizeof(in));
    }
    strcpy(in.version, "0.7.0");
    in.heap = 91234; in.uptime_s = 93784; in.mgmt_port = 28480;

    /* client: connected, device known */
    in.mode = UI_CLIENT; in.wifi_state = 2; strcpy(in.wifi_ssid, strcmp(lang, "uk") ? "Home network 5G" : "Домашня мережа 5G");  /* cut: too long */ in.rssi = -57;
    in.mgmt_ip = in.dev_ip = ip(192, 168, 1, 50); in.eth_up = true; in.dev_known = true;
    in.eth_speed = 100; in.eth_full = true;
    memcpy(in.dev_mac, "\x00\x11\x22\x33\x44\x55", 6);
    in.to_wifi_bytes = 3456789; in.to_eth_bytes = 51200; in.dropped = 3; in.tx_errors = 0;
    assert(ui_page_count(&in) == 4);
    all_pages(&in, "client");

    /* client: script outputs add a page */
    in.n_out = 2;
    strcpy(in.out_key[0], "Режим"); strcpy(in.out_val[0], "клієнт");
    /* a key cut mid-character, as a 24-byte buffer would store it */
    memcpy(in.out_key[1], "Дуже довгий ключ", 23); in.out_key[1][23] = '\0';     /* 23 bytes: mid "й" */
    snprintf(in.out_val[1], sizeof(in.out_val[1]), "%s", "і ще довше значення, яке не влізе");
    assert(ui_page_count(&in) == 5);
    all_pages(&in, "client_script");
    in.n_out = 0;

    /* client: not configured, setup AP */
    in.wifi_state = 0; in.mgmt_ip = 0; in.dev_ip = 0; in.dev_known = false; in.eth_up = false;
    in.setup_ap = true; strcpy(in.setup_ssid, "WT32-Setup-1A2B");
    all_pages(&in, "client_setup");

    /* one-time setup boot */
    in.setup_boot = true; in.wifi_state = 1;
    all_pages(&in, "setup_boot");
    in.setup_boot = false;

    /* router */
    memset(&in.wifi_ssid, 0, sizeof(in.wifi_ssid));
    in.mode = UI_ROUTER; strcpy(in.ap_ssid, "WT32-1A2B"); in.ap_clients = 2; in.wt32_ip = ip(192, 168, 77, 1);
    in.eth_up = true; in.dev_known = true; in.dev_ip = 0;
    assert(ui_page_count(&in) == 3);
    all_pages(&in, "router_wait");
    in.dev_ip = ip(192, 168, 77, 100);
    all_pages(&in, "router");

    /* access point: DHCP, waiting, fallback */
    in.mode = UI_AP; in.uplink = UI_UPLINK_DHCP; in.wt32_ip = ip(192, 168, 1, 23); in.gw = ip(192, 168, 1, 1);
    all_pages(&in, "ap");
    in.uplink = UI_UPLINK_WAITING; in.wt32_ip = 0; in.gw = 0;
    all_pages(&in, "ap_wait");
    in.uplink = UI_UPLINK_FALLBACK; in.wt32_ip = ip(192, 168, 77, 1);
    all_pages(&in, "ap_fallback");
    in.uplink = 77;                                   /* garbage must not index out of range */
    all_pages(&in, "ap_bad");

    /* button hold screens and notices */
    uint32_t holds[] = { 1200, 3000, 6000, 12000 };
    for (int i = 0; i < 4; i++) {
        ui_render_hold(fb, holds[i]);
        strips();
        char nm[32]; snprintf(nm, sizeof(nm), "hold_%lu", (unsigned long)holds[i]);
        save(nm);
    }
    ui_render_notice(fb, ui_tr("disp.tReset"), ui_tr("disp.erasing"), ui_tr("disp.restarting"), NULL);
    strips();
    save("notice");

    /* label and value on one line: the longest values still fit */
    {
        char b[96];
        ui_trf(b, sizeof(b), "disp.mac", "v", "AA:BB:CC:DD:EE:FF", NULL);
        assert(ui_utf8_len(b) <= UI_COLS);
        ui_trf(b, sizeof(b), "disp.ip", "v", ui_tr("disp.waitDhcp"), NULL);
        assert(ui_utf8_len(b) <= UI_COLS);
        ui_trf(b, sizeof(b), "disp.dhcp", "v", ui_tr("disp.uplinkFallback"), NULL);
        assert(ui_utf8_len(b) <= UI_COLS);
    }

}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : NULL;

    /* font table is sorted (binary search) */
    for (size_t i = 1; i < font6x10_count; i++) assert(font6x10[i - 1].cp < font6x10[i].cp);

    /* UTF-8 */
    uint32_t cp;
    assert(ui_utf8_next("A", &cp) == 1 && cp == 'A');
    assert(ui_utf8_next("Ї", &cp) == 2 && cp == 0x407);
    assert(ui_utf8_next("…", &cp) == 3 && cp == 0x2026);
    assert(ui_utf8_next("\xF0\x9F\x98\x80", &cp) == 4 && cp == 0x1F600);
    assert(ui_utf8_next("\x80z", &cp) == 1 && cp == '?');           /* stray continuation */
    assert(ui_utf8_next("\xD0", &cp) == 1 && cp == '?');            /* cut at the end */
    assert(ui_utf8_next("\xE2\x80z", &cp) == 2 && cp == '?');
    assert(ui_utf8_next("", &cp) == 0);
    assert(ui_utf8_len("Привіт, WT32") == 12);
    char t1[] = "abcЇ", t2[] = "abc\xD0", t3[] = "ab\xE2\x80", t4[] = "\x80\x80";
    ui_utf8_trim(t1); ui_utf8_trim(t2); ui_utf8_trim(t3); ui_utf8_trim(t4);
    assert(!strcmp(t1, "abcЇ") && !strcmp(t2, "abc") && !strcmp(t3, "ab") && !strcmp(t4, "\x80\x80"));

    /* cutting */
    memset(fb, 0, sizeof(fb));
    assert(ui_text(fb, 0, 0, "Коротко", 21, false) == 7);
    assert(ui_text(fb, 0, 0, "дуже-дуже довгий рядок тексту", 21, false) == 21);
    assert(ui_text(fb, 0, 0, "abc", 0, false) == 0);
    assert(ui_text(fb, 120, 60, "край екрана", 21, false) == 11);  /* clipped, no overrun */
    memset(fb, 0, sizeof(fb));
    ui_text(fb, 0, 0, "\xFF\xFE unknown \xF0\x9F\x98\x80", 21, false);   /* unknown -> '?' glyph */
    assert(lit(0, 0, 6, 10) > 0);

    /* texts: the "disp.*" keys of a flat JSON object, {name} placeholders */
    assert(!strcmp(ui_tr("disp.tNetwork"), "disp.tNetwork"));          /* none yet: the key */
    const char *j1 = " { \"page.x\" : \"no\", \"disp.a\":\"Tab\\t \\\"q\\\" \\u0406\\u2026\",\n"
                     "  \"disp.f\": \"Mode {m}: {n} of {n} {x}\" } ";
    assert(ui_set_texts(j1, strlen(j1)) == 2);
    assert(!strcmp(ui_tr("disp.a"), "Tab\t \"q\" І…"));
    assert(!strcmp(ui_tr("page.x"), "page.x"));                        /* only disp.* kept */
    char o[40];
    ui_trf(o, sizeof(o), "disp.f", "n", "5", "m", "ap", NULL);
    assert(!strcmp(o, "Mode ap: 5 of 5 {x}"));                        /* unknown placeholder stays */
    ui_trf(o, 9, "disp.f", "m", "Режим", NULL);                        /* cut: not mid-character */
    assert(!strcmp(o, "Mode Р"));                                     /* 8 bytes: "Р" + half of "е" dropped */
    ui_trf(o, sizeof(o), "disp.none", NULL);
    assert(!strcmp(o, "disp.none"));
    const char *bad[] = { "", "[]", "{\"disp.a\":1}", "{\"disp.a\":\"x\"", "{\"disp.a\":\"x\",}", "{\"disp.a\" \"x\"}",
                          "{\"disp.a\":\"\\u12\"}", "{\"disp.a\":\"x" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(ui_set_texts(bad[i], strlen(bad[i])) == -1);
        assert(!strcmp(ui_tr("disp.a"), "Tab\t \"q\" І…"));           /* kept on error */
    }
    assert(ui_set_texts("{}", 2) == 0 && !strcmp(ui_tr("disp.a"), "disp.a"));

    static const char *const eth_en[4] = { "Ethernet: no link", "Ethernet: link up", "Ethernet: 100M full", "Ethernet: 10M half" };
    static const char *const eth_uk[4] = { "Ethernet: немає лінку", "Ethernet: є лінк", "Ethernet: 100М повний", "Ethernet: 10М напів" };
    pages_in("en", eth_en);
    pages_in("uk", eth_uk);

    puts("all display tests passed");
    return 0;
}
