/* Wi-Fi setup web server and captive-portal DNS. See setup_portal.h. */
#include "setup_portal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "portal_auth.h"
#include "portal_ota.h"
#include "wifi_setup.h"

#define SCAN_MAX      20
#define FORM_MAX      256
#define RESTART_DELAY_US (1500LL * 1000)

static const char *TAG = "portal";

static setup_portal_config_t s_cfg;

extern const char auth_js_start[] asm("_binary_auth_js_start");
extern const char auth_js_end[] asm("_binary_auth_js_end");
extern const char ota_js_start[] asm("_binary_ota_js_start");
extern const char ota_js_end[] asm("_binary_ota_js_end");

/* ---------- helpers ---------- */

/* snprintf that never lets pos run past the buffer. */
size_t setup_portal_appendf(char *buf, size_t pos, size_t cap, const char *fmt, ...)
{
    if (pos >= cap) {
        return pos;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return pos;
    }
    pos += n;
    return pos < cap ? pos : cap - 1;
}

/* Append s to buf as a JSON string literal (with quotes). */
size_t setup_portal_json_str(char *buf, size_t pos, size_t cap, const char *s)
{
    if (pos < cap) buf[pos++] = '"';
    for (; *s && pos + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = c;
        } else if (c < 0x20) {
            pos = setup_portal_appendf(buf, pos, cap, "\\u%04x", c);
        } else {
            buf[pos++] = c;
        }
    }
    if (pos < cap) buf[pos++] = '"';
    if (pos < cap) buf[pos] = '\0';
    return pos;
}

static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

/* Read a small urlencoded body. */
esp_err_t setup_portal_read_form(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

esp_err_t setup_portal_form_value(const char *form, const char *key, char *val, size_t cap)
{
    esp_err_t err = httpd_query_key_value(form, key, val, cap);
    if (err == ESP_ERR_NOT_FOUND) {
        val[0] = '\0';
        return ESP_OK;
    }
    if (err == ESP_OK) {
        url_decode(val);
    }
    return err;
}

esp_err_t setup_portal_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t setup_portal_send_error_key(httpd_req_t *req, const char *key, const char *en)
{
    char buf[384];
    size_t pos = setup_portal_appendf(buf, 0, sizeof(buf), "{\"ok\":false,\"key\":");
    pos = setup_portal_json_str(buf, pos, sizeof(buf) - 2, key);
    pos = setup_portal_appendf(buf, pos, sizeof(buf) - 2, ",\"message\":");
    pos = setup_portal_json_str(buf, pos, sizeof(buf) - 2, en);
    strcpy(buf + pos, "}");
    httpd_resp_set_status(req, "400 Bad Request");
    return setup_portal_send_json(req, buf);
}

esp_err_t setup_portal_send_msg(httpd_req_t *req, portal_msg_t msg)
{
    return setup_portal_send_error_key(req, msg.key, msg.en);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

void setup_portal_restart_later(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
        esp_timer_create(&args, &t);
    }
    if (s_cfg.before_restart) {
        s_cfg.before_restart();
    }
    esp_timer_start_once(t, RESTART_DELAY_US);
}

/* ---------- handlers ---------- */

static esp_err_t auth_js_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_send(req, auth_js_start, auth_js_end - auth_js_start - 1);
}

static esp_err_t ota_js_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_send(req, ota_js_start, ota_js_end - ota_js_start - 1);
}

/* ---------- the device's language ---------- */

#define LANG_NS     "wifi_setup"
#define LANG_KEY    "lang"

static const portal_lang_t *lang_find(const char *code)
{
    for (size_t i = 0; code && i < portal_lang_count; i++) {
        if (strcmp(portal_langs[i].code, code) == 0) {
            return &portal_langs[i];
        }
    }
    return NULL;
}

/* The saved language, NULL if none (or no longer built in). */
static const portal_lang_t *lang_saved(void)
{
    char code[12] = "";
    size_t len = sizeof(code);
    nvs_handle_t h;
    if (nvs_open(LANG_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, LANG_KEY, code, &len) != ESP_OK) {
            code[0] = '\0';
        }
        nvs_close(h);
    }
    return lang_find(code);
}

const portal_lang_t *setup_portal_lang(void)
{
    const portal_lang_t *l = lang_saved();
    return l ? l : (portal_lang_count ? &portal_langs[0] : NULL);
}

static esp_err_t lang_post(httpd_req_t *req)
{
    char form[48], code[12];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "lang", code, sizeof(code)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.malformedRequest", "Malformed request");
    }
    const portal_lang_t *lang = lang_find(code);
    if (!lang) {
        return setup_portal_send_error_key(req, "err.unknownLanguage", "Unknown language");
    }
    if (lang != lang_saved()) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(LANG_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_str(h, LANG_KEY, lang->code);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err != ESP_OK) {
            return setup_portal_send_error_key(req, "err.couldNotSave", "Could not save");
        }
        ESP_LOGI(TAG, "language: %s", lang->code);
        if (s_cfg.on_lang) {
            s_cfg.on_lang(lang);
        }
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

/* {"lang":"uk","device":"uk","saved":true,"langs":[{"code","name","label"},...],"strings":{...}}
 * for ?l=<code>; without it (or unknown) the device's language: the saved one,
 * else ?b=<code> (the browser's), else the first. Public: the login form needs it. */
static esp_err_t i18n_get(httpd_req_t *req)
{
    const portal_lang_t *saved = lang_saved();
    const portal_lang_t *device = saved ? saved : (portal_lang_count ? &portal_langs[0] : NULL);
    const portal_lang_t *lang = NULL;
    char q[48], code[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "l", code, sizeof(code)) == ESP_OK) {
            lang = lang_find(code);
        }
        if (!lang && !saved && httpd_query_key_value(q, "b", code, sizeof(code)) == ESP_OK) {
            lang = lang_find(code);
        }
    }
    if (!lang) {
        lang = device;
    }
    char head[1024];
    size_t pos = setup_portal_appendf(head, 0, sizeof(head), "{\"lang\":");
    pos = setup_portal_json_str(head, pos, sizeof(head), lang ? lang->code : "");
    pos = setup_portal_appendf(head, pos, sizeof(head), ",\"device\":");
    pos = setup_portal_json_str(head, pos, sizeof(head), device ? device->code : "");
    pos = setup_portal_appendf(head, pos, sizeof(head), ",\"saved\":%s,\"langs\":[", saved ? "true" : "false");
    for (size_t i = 0; i < s_cfg.lang_count; i++) {
        pos = setup_portal_appendf(head, pos, sizeof(head), "%s{\"code\":", i ? "," : "");
        pos = setup_portal_json_str(head, pos, sizeof(head), s_cfg.langs[i].code);
        pos = setup_portal_appendf(head, pos, sizeof(head), ",\"name\":");
        pos = setup_portal_json_str(head, pos, sizeof(head), s_cfg.langs[i].name);
        pos = setup_portal_appendf(head, pos, sizeof(head), ",\"label\":");
        pos = setup_portal_json_str(head, pos, sizeof(head), s_cfg.langs[i].label);
        pos = setup_portal_appendf(head, pos, sizeof(head), "}");
    }
    pos = setup_portal_appendf(head, pos, sizeof(head), "],\"strings\":");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send_chunk(req, head, pos);
    if (err == ESP_OK) {
        err = lang ? httpd_resp_send_chunk(req, lang->json, lang->json_len) : httpd_resp_send_chunk(req, "{}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "}", 1);
    }
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_cfg.page, s_cfg.page_len);
}

#ifndef PORTAL_GIT_COMMIT
#define PORTAL_GIT_COMMIT "unknown"
#endif

/* ,"build":{...}: which firmware this is, exactly. */
static size_t build_json(char *buf, size_t pos, size_t cap)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char elf[17];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    pos = setup_portal_appendf(buf, pos, cap, ",\"build\":{\"project\":");
    pos = setup_portal_json_str(buf, pos, cap, d->project_name);
    pos = setup_portal_appendf(buf, pos, cap, ",\"version\":");
    pos = setup_portal_json_str(buf, pos, cap, d->version);
    pos = setup_portal_appendf(buf, pos, cap, ",\"date\":");
    pos = setup_portal_json_str(buf, pos, cap, d->date);
    pos = setup_portal_appendf(buf, pos, cap, ",\"time\":");
    pos = setup_portal_json_str(buf, pos, cap, d->time);
    pos = setup_portal_appendf(buf, pos, cap, ",\"commit\":");
    pos = setup_portal_json_str(buf, pos, cap, PORTAL_GIT_COMMIT);
    pos = setup_portal_appendf(buf, pos, cap, ",\"elf\":\"%s\",\"idf\":", elf);
    pos = setup_portal_json_str(buf, pos, cap, d->idf_ver);
    return setup_portal_appendf(buf, pos, cap, "}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    static const char *states[] = {
        [WIFI_SETUP_UNCONFIGURED] = "setup",
        [WIFI_SETUP_CONNECTING] = "connecting",
        [WIFI_SETUP_CONNECTED] = "connected",
    };
    char ip[16];
    wifi_setup_ip(ip, sizeof(ip));

    const size_t cap = 4608;    /* room for the application fields, e.g. 32 DHCP leases */
    char *buf = malloc(cap);    /* two servers may answer at once: no shared buffer */
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    size_t pos = setup_portal_appendf(buf, 0, cap, "{\"state\":\"%s\",\"ssid\":", states[wifi_setup_state()]);
    pos = setup_portal_json_str(buf, pos, cap, wifi_setup_ssid());
    pos = setup_portal_appendf(buf, pos, cap, ",\"host\":");
    pos = setup_portal_json_str(buf, pos, cap, wifi_setup_hostname());
    pos = setup_portal_appendf(buf, pos, cap, ",\"apSsid\":");
    pos = setup_portal_json_str(buf, pos, cap, wifi_setup_ap_ssid());
    pos = setup_portal_appendf(buf, pos, cap,
                               ",\"ap\":%s,\"ip\":\"%s\",\"rssi\":%d,\"defaultPassword\":%s,\"version\":\"%s\"",
                               wifi_setup_ap_active() ? "true" : "false", ip, wifi_setup_rssi(),
                               portal_auth_is_default() ? "true" : "false",
                               esp_app_get_description()->version);
    pos = build_json(buf, pos, cap - 2);
    if (s_cfg.ota) {
        pos = portal_ota_status(buf, pos, cap - 2);
    }
    if (s_cfg.status_extra) {
        pos = s_cfg.status_extra(buf, pos, cap - 2);
    }
    setup_portal_appendf(buf, pos, cap, "}");
    esp_err_t err = setup_portal_send_json(req, buf);
    free(buf);
    return err;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_ap_record_t *aps = calloc(SCAN_MAX, sizeof(*aps));
    char *buf = malloc(SCAN_MAX * 96 + 16);
    if (!aps || !buf) {
        free(aps);
        free(buf);
        return httpd_resp_send_500(req);
    }
    int n = wifi_setup_scan(aps, SCAN_MAX);
    size_t cap = SCAN_MAX * 96 + 16;
    size_t pos = setup_portal_appendf(buf, 0, cap, "[");
    for (int i = 0; i < n; i++) {
        if (!aps[i].ssid[0]) {
            continue;
        }
        pos = setup_portal_appendf(buf, pos, cap, "%s{\"ssid\":", pos > 1 ? "," : "");
        pos = setup_portal_json_str(buf, pos, cap, (const char *)aps[i].ssid);
        pos = setup_portal_appendf(buf, pos, cap, ",\"rssi\":%d,\"open\":%s}", aps[i].rssi,
                        aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    setup_portal_appendf(buf, pos, cap, "]");
    esp_err_t err = setup_portal_send_json(req, buf);
    free(aps);
    free(buf);
    return err;
}

static bool hostname_valid(const char *h)
{
    size_t len = strlen(h);
    if (len == 0 || len > 32 || h[0] == '-' || h[len - 1] == '-') {
        return false;
    }
    for (; *h; h++) {
        char c = *h;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char form[FORM_MAX], ssid[33], pass[65], host[33];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        setup_portal_form_value(form, "pass", pass, sizeof(pass)) != ESP_OK ||
        setup_portal_form_value(form, "host", host, sizeof(host)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.formDataTooLongOr", "Form data too long or malformed");
    }
    if (!ssid[0]) {
        return setup_portal_send_error_key(req, "err.chooseANetwork", "Choose a network");
    }
    size_t plen = strlen(pass);
    if (plen != 0 && (plen < 8 || plen > 63)) {
        return setup_portal_send_error_key(req, "err.theWiFiPasswordMust", "The Wi-Fi password must be 8–63 characters (or empty for an open network)");
    }
    if (host[0] && !hostname_valid(host)) {
        return setup_portal_send_error_key(req, "err.deviceNameAZ0", "Device name: a–z, 0–9 and hyphen only, up to 32 characters");
    }
    if (wifi_setup_save(ssid, pass, host) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.couldNotSaveTheSettings", "Could not save the settings");
    }
    ESP_LOGI(TAG, "saved network \"%s\", restarting", ssid);
    setup_portal_restart_later();
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    if (wifi_setup_forget() != ESP_OK) {
        return setup_portal_send_error_key(req, "err.couldNotEraseTheSettings", "Could not erase the settings");
    }
    ESP_LOGI(TAG, "network forgotten, restarting into setup");
    setup_portal_restart_later();
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static httpd_handle_t s_servers[2];     /* [0] port 80, [1] LAN port (optional) */

/* Marks the port-80 server, whose 404s become captive-portal redirects. */
static char s_captive_mark;

static void no_free(void *ctx)
{
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    bool captive = httpd_get_global_user_ctx(req->handle) == &s_captive_mark;
    if (!captive || !wifi_setup_ap_active()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }
    /* OS connectivity checks (generate_204, hotspot-detect.html, ...) land
     * here; the redirect makes phones open the setup page by themselves. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t start_server(int idx, uint16_t port, uint16_t ctrl_port, bool captive)
{
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = port;
    hcfg.ctrl_port = ctrl_port;
    hcfg.lru_purge_enable = true;
    hcfg.stack_size = 6144;
    hcfg.max_uri_handlers = 32;
    if (s_cfg.lan_port) {
        hcfg.max_open_sockets = 3;          /* two servers share the lwIP socket pool */
    }
    hcfg.global_user_ctx = captive ? &s_captive_mark : NULL;
    hcfg.global_user_ctx_free_fn = no_free;

    esp_err_t err = httpd_start(&s_servers[idx], &hcfg);
    if (err != ESP_OK) {
        return err;
    }
    httpd_register_err_handler(s_servers[idx], HTTPD_404_NOT_FOUND, not_found);
    return ESP_OK;
}

static esp_err_t register_raw(const httpd_uri_t *uri)
{
    for (int i = 0; i < 2; i++) {
        if (s_servers[i]) {
            esp_err_t err = httpd_register_uri_handler(s_servers[i], uri);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

typedef struct {
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;
} guarded_t;

/* Every route except the page, its script and login needs a session. */
static esp_err_t guarded(httpd_req_t *req)
{
    const guarded_t *g = req->user_ctx;
    if (!portal_auth_session_valid(req)) {
        return portal_auth_reject(req);
    }
    req->user_ctx = g->user_ctx;
    return g->handler(req);
}

esp_err_t setup_portal_register_public(const httpd_uri_t *uri)
{
    return register_raw(uri);
}

esp_err_t setup_portal_register(const httpd_uri_t *uri)
{
    guarded_t *g = malloc(sizeof(*g));     /* lives as long as the servers */
    if (!g) {
        return ESP_ERR_NO_MEM;
    }
    g->handler = uri->handler;
    g->user_ctx = uri->user_ctx;
    httpd_uri_t wrapped = *uri;
    wrapped.handler = guarded;
    wrapped.user_ctx = g;
    return register_raw(&wrapped);
}

esp_err_t setup_portal_start(const setup_portal_config_t *cfg)
{
    s_cfg = *cfg;
    esp_err_t err = portal_auth_init();
    if (err == ESP_OK) {
        err = start_server(0, 80, 32768, true);
    }
    if (err == ESP_OK && s_cfg.lan_port) {
        err = start_server(1, s_cfg.lan_port, 32769, false);
    }
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t open_uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = page_get },
        { .uri = "/auth.js",     .method = HTTP_GET,  .handler = auth_js_get },
        { .uri = "/i18n.json",   .method = HTTP_GET,  .handler = i18n_get },
        { .uri = "/api/login",   .method = HTTP_POST, .handler = portal_auth_login_post },
    };
    const httpd_uri_t uris[] = {
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/forget",   .method = HTTP_POST, .handler = forget_post },
        { .uri = "/api/logout",   .method = HTTP_POST, .handler = portal_auth_logout_post },
        { .uri = "/api/password", .method = HTTP_POST, .handler = portal_auth_password_post },
        { .uri = "/api/lang",     .method = HTTP_POST, .handler = lang_post },
    };
    for (size_t i = 0; i < sizeof(open_uris) / sizeof(open_uris[0]) && err == ESP_OK; i++) {
        err = register_raw(&open_uris[i]);
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]) && err == ESP_OK; i++) {
        err = setup_portal_register(&uris[i]);
    }
    if (err == ESP_OK && s_cfg.ota) {
        const httpd_uri_t js = { .uri = "/ota.js", .method = HTTP_GET, .handler = ota_js_get };
        const httpd_uri_t post = { .uri = "/api/ota", .method = HTTP_POST, .handler = portal_ota_post };
        err = register_raw(&js);
        if (err == ESP_OK) {
            err = setup_portal_register(&post);
        }
        portal_ota_init();
    }
    return err;
}

/* ---------- captive DNS ---------- */

static volatile bool s_dns_active;
static volatile uint32_t s_dns_ip;
static TaskHandle_t s_dns_task;

/* Turn the query in buf into an answer; returns the reply length or 0. */
static size_t dns_answer(uint8_t *buf, size_t len, size_t cap, uint32_t ip)
{
    if (len < 12 || (buf[2] & 0x80) || ((buf[4] << 8) | buf[5]) != 1) {
        return 0;   /* not a single-question query */
    }
    size_t p = 12;
    while (p < len && buf[p] != 0) {
        if (buf[p] & 0xC0) {
            return 0;   /* compression is not expected in a question */
        }
        p += buf[p] + 1;
    }
    if (p + 5 > len) {
        return 0;
    }
    uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
    size_t out = p + 5;   /* end of question; drop any EDNS record after it */

    buf[2] = 0x84 | (buf[2] & 0x01);   /* QR, AA, keep RD */
    buf[3] = 0x80;                      /* RA, NOERROR */
    memset(buf + 6, 0, 6);              /* AN/NS/AR counts */

    if (qtype == 1 /* A */ && out + 16 <= cap) {
        const uint8_t rr[] = {
            0xC0, 0x0C,             /* name: pointer to question */
            0x00, 0x01, 0x00, 0x01, /* type A, class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60 s */
            0x00, 0x04,
        };
        memcpy(buf + out, rr, sizeof(rr));
        memcpy(buf + out + sizeof(rr), &ip, 4);
        out += sizeof(rr) + 4;
        buf[7] = 1;
    }
    return out;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        if (sock >= 0) close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    static uint8_t buf[512];
    while (true) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0 || !s_dns_active) {
            continue;
        }
        size_t out = dns_answer(buf, n, sizeof(buf), s_dns_ip);
        if (out) {
            sendto(sock, buf, out, 0, (struct sockaddr *)&from, flen);
        }
    }
}

void setup_portal_dns_start(uint32_t ap_ip)
{
    s_dns_ip = ap_ip;
    s_dns_active = true;
    if (!s_dns_task) {
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);
    }
}

void setup_portal_dns_stop(void)
{
    s_dns_active = false;
}
