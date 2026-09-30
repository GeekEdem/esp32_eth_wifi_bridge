/* Web server with the Wi-Fi setup API and the captive-portal DNS.
 *
 * Core routes (every API route except login needs a session, see portal_auth.h):
 *   GET  /               application page (should load /auth.js)
 *   GET  /auth.js        login form and password section
 *   POST /api/login      password -> session cookie
 *   POST /api/logout
 *   POST /api/password   old, new
 *   GET  /api/status     JSON status (+ application fields)
 *   GET  /api/scan       visible networks
 *   POST /api/wifi       ssid, pass, host -> save and restart
 *   POST /api/forget     forget the network and restart into setup
 *   GET  /ota.js, POST /api/ota   firmware update (with .ota, see portal_ota.h)
 *   anything else        302 to the page while the setup AP is up
 * With lan_port set, a second server serves the same routes on that port
 * (used where port 80 belongs to someone else, see the transparent bridge).
 * Application routes go through setup_portal_register().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    const char *page;               /* UTF-8 HTML */
    size_t page_len;
    /* Append ",\"key\":value" pairs to the status object; returns new pos. */
    size_t (*status_extra)(char *buf, size_t pos, size_t cap);
    void (*before_restart)(void);
    uint16_t lan_port;              /* 0 = port 80 only */
    bool ota;                       /* firmware update route + rollback confirmation */
} setup_portal_config_t;

esp_err_t setup_portal_start(const setup_portal_config_t *cfg);

/* Register an application route on every running server; it requires a
 * logged-in session. */
esp_err_t setup_portal_register(const httpd_uri_t *uri);

/* Register a route without the session check (static assets only). */
esp_err_t setup_portal_register_public(const httpd_uri_t *uri);

/* Back to the default password (e.g. from a long button press). */
esp_err_t setup_portal_reset_password(void);

/* DNS that answers every A query with ap_ip (network byte order).
 * Answers only while active; driven by wifi_setup. */
void setup_portal_dns_start(uint32_t ap_ip);
void setup_portal_dns_stop(void);

/* Helpers for application handlers. */
size_t setup_portal_appendf(char *buf, size_t pos, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
size_t setup_portal_json_str(char *buf, size_t pos, size_t cap, const char *s);
esp_err_t setup_portal_send_json(httpd_req_t *req, const char *json);
esp_err_t setup_portal_send_error(httpd_req_t *req, const char *message);
esp_err_t setup_portal_read_form(httpd_req_t *req, char *buf, size_t cap);
/* Missing key -> "", too long -> error. Value is URL-decoded. */
esp_err_t setup_portal_form_value(const char *form, const char *key, char *val, size_t cap);
void setup_portal_restart_later(void);
