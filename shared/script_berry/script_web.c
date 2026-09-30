/* Script routes on the setup portal (all but /script.js need a session):
 *   GET  /script.js
 *   GET  /api/script              saved text (text/plain); the example if none
 *   POST /api/script[?run=1]      raw text body -> save (stops the script), optionally run
 *   POST /api/script/run | /api/script/stop
 *   POST /api/script/autostart    on=1|0
 *   GET  /api/script/state[?since=N]
 */
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "script.h"
#include "script_internal.h"
#include "setup_portal.h"

#define READ_CHUNK 1024

extern const char script_js_start[] asm("_binary_script_js_start");
extern const char script_js_end[] asm("_binary_script_js_end");

static bool query_flag(httpd_req_t *req, const char *key, char *val, size_t cap)
{
    char q[64];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, val, cap) == ESP_OK;
}

static esp_err_t js_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_send(req, script_js_start, script_js_end - script_js_start - 1);
}

static esp_err_t text_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = script_saved_len();
    if (!len) {
        httpd_resp_set_hdr(req, "X-Script-Saved", "0");
        return httpd_resp_sendstr(req, script_example());
    }
    httpd_resp_set_hdr(req, "X-Script-Saved", "1");
    char *buf = malloc(READ_CHUNK);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < len && err == ESP_OK; off += READ_CHUNK) {
        size_t n = len - off < READ_CHUNK ? len - off : READ_CHUNK;
        err = script_read(off, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    free(buf);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t text_post(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len > script_max_len()) {
        return setup_portal_send_error(req, "Скрипт завеликий");
    }
    char *src = malloc(len + 1);
    if (!src) {
        return setup_portal_send_error(req, "Бракує памʼяті для прийому скрипта");
    }
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, src + got, len - got);
        if (r <= 0) {
            free(src);
            return setup_portal_send_error(req, "Передачу перервано");
        }
        got += r;
    }
    esp_err_t err = script_save(src, len);
    free(src);
    if (err != ESP_OK) {
        return setup_portal_send_error(req, "Не вдалося зберегти скрипт");
    }
    char run[4];
    if (query_flag(req, "run", run, sizeof(run)) && strcmp(run, "1") == 0) {
        script_run();
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t run_post(httpd_req_t *req)
{
    if (!script_saved_len()) {
        return setup_portal_send_error(req, "Скрипт ще не збережено");
    }
    return script_run() == ESP_OK ? setup_portal_send_json(req, "{\"ok\":true}")
                                  : setup_portal_send_error(req, "Зайнято, спробуйте ще раз");
}

static esp_err_t stop_post(httpd_req_t *req)
{
    return script_stop() == ESP_OK ? setup_portal_send_json(req, "{\"ok\":true}")
                                   : setup_portal_send_error(req, "Зайнято, спробуйте ще раз");
}

static esp_err_t autostart_post(httpd_req_t *req)
{
    char form[16], on[4];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "on", on, sizeof(on)) != ESP_OK) {
        return setup_portal_send_error(req, "Пошкоджений запит");
    }
    if (script_set_autostart(strcmp(on, "1") == 0) != ESP_OK) {
        return setup_portal_send_error(req, "Не вдалося зберегти");
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t state_get(httpd_req_t *req)
{
    char since_s[12];
    uint32_t since = 0;
    if (query_flag(req, "since", since_s, sizeof(since_s))) {
        since = strtoul(since_s, NULL, 10);
    }
    const size_t cap = 8192;           /* 40 console lines of up to ~120 chars, escaped */
    char *buf = malloc(cap);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    size_t pos = setup_portal_appendf(buf, 0, cap, "{\"ok\":true,");
    pos = script_state_json(buf, pos, cap - 2, since);
    setup_portal_appendf(buf, pos, cap, "}");
    esp_err_t err = setup_portal_send_json(req, buf);
    free(buf);
    return err;
}

esp_err_t script_web_start(void)
{
    const httpd_uri_t js = { .uri = "/script.js", .method = HTTP_GET, .handler = js_get };
    const httpd_uri_t uris[] = {
        { .uri = "/api/script",           .method = HTTP_GET,  .handler = text_get },
        { .uri = "/api/script",           .method = HTTP_POST, .handler = text_post },
        { .uri = "/api/script/run",       .method = HTTP_POST, .handler = run_post },
        { .uri = "/api/script/stop",      .method = HTTP_POST, .handler = stop_post },
        { .uri = "/api/script/autostart", .method = HTTP_POST, .handler = autostart_post },
        { .uri = "/api/script/state",     .method = HTTP_GET,  .handler = state_get },
    };
    esp_err_t err = setup_portal_register_public(&js);
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]) && err == ESP_OK; i++) {
        err = setup_portal_register(&uris[i]);
    }
    return err;
}
