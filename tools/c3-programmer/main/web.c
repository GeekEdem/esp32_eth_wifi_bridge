/* C3 web page: shared Wi-Fi setup plus
 *   POST /api/target   action=reset|boot
 * and RFC2217/target fields in /api/status. */
#include "web.h"

#include <string.h>

#include "bridge.h"
#include "sdkconfig.h"
#include "setup_portal.h"
#include "target_ctl.h"

extern const char page_start[] asm("_binary_portal_html_start");
extern const char page_end[] asm("_binary_portal_html_end");

static size_t status_extra(char *buf, size_t pos, size_t cap)
{
    return setup_portal_appendf(buf, pos, cap, ",\"port\":%d,\"client\":%s,\"en\":%s,\"boot\":%s",
                                CONFIG_C3PROG_RFC2217_PORT,
                                bridge_client_connected() ? "true" : "false",
                                target_en_asserted() ? "true" : "false",
                                target_boot_asserted() ? "true" : "false");
}

static esp_err_t target_post(httpd_req_t *req)
{
    char form[64], action[16];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "action", action, sizeof(action)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.malformedRequest", "Malformed request");
    }
    if (bridge_client_connected()) {
        return setup_portal_send_error_key(req, "err.anRfc2217SessionIsActive", "An RFC2217 session is active: close it first");
    }
    if (strcmp(action, "reset") == 0) {
        target_reset();
    } else if (strcmp(action, "boot") == 0) {
        target_enter_bootloader();
    } else {
        return setup_portal_send_error_key(req, "err.unknownAction", "Unknown action");
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

esp_err_t web_start(void)
{
    const setup_portal_config_t cfg = {
        .page = page_start,
        .page_len = page_end - page_start - 1,   /* EMBED_TXTFILES adds a NUL */
        .status_extra = status_extra,
        .ota = true,
        .langs = portal_langs,
        .lang_count = portal_lang_count,
        .before_restart = target_release,
    };
    esp_err_t err = setup_portal_start(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t target = { .uri = "/api/target", .method = HTTP_POST, .handler = target_post };
    return setup_portal_register(&target);
}
