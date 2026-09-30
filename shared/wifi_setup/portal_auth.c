/* Web page password and sessions. See portal_auth.h. */
#include "portal_auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "setup_portal.h"

#define NVS_NS          "wifi_setup"
#define NVS_KEY         "pw"
#define SALT_LEN        16
#define HASH_LEN        32
#define MAX_SESSIONS    4
#define SESSION_IDLE_US (30LL * 60 * 1000 * 1000)
#define MAX_FAILS       5
#define LOCKOUT_US      (30LL * 1000 * 1000)
#define PW_MIN          8
#define PW_MAX          63

static const char *TAG = "auth";

typedef struct {
    char id[33];                /* 128-bit hex; "" = free */
    int64_t last_us;
} session_t;

static SemaphoreHandle_t s_lock;
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static bool s_default;
static session_t s_sessions[MAX_SESSIONS];
static int s_fails;
static int64_t s_locked_until_us;

static void hash_password(const uint8_t salt[SALT_LEN], const char *pw, uint8_t out[HASH_LEN])
{
    uint8_t buf[SALT_LEN + PW_MAX + 1];
    size_t n = strnlen(pw, PW_MAX);
    memcpy(buf, salt, SALT_LEN);
    memcpy(buf + SALT_LEN, pw, n);
    size_t olen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, buf, SALT_LEN + n, out, HASH_LEN, &olen) != PSA_SUCCESS ||
        olen != HASH_LEN) {
        esp_fill_random(out, HASH_LEN);     /* matches nothing */
    }
    memset(buf, 0, sizeof(buf));
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

/* Default password: a fixed hash computed at start, never stored. */
static void use_default(void)
{
    esp_fill_random(s_salt, SALT_LEN);
    hash_password(s_salt, PORTAL_DEFAULT_PASSWORD, s_hash);
    s_default = true;
}

esp_err_t portal_auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    nvs_handle_t h;
    uint8_t blob[SALT_LEN + HASH_LEN];
    size_t len = sizeof(blob);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, NVS_KEY, blob, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(blob)) {
            memcpy(s_salt, blob, SALT_LEN);
            memcpy(s_hash, blob + SALT_LEN, HASH_LEN);
            s_default = false;
            return ESP_OK;
        }
    }
    use_default();
    ESP_LOGW(TAG, "web page uses the default password");
    return ESP_OK;
}

bool portal_auth_is_default(void)
{
    return s_default;
}

static bool password_ok(const char *pw)
{
    uint8_t h[HASH_LEN];
    hash_password(s_salt, pw, h);
    return ct_equal(h, s_hash, HASH_LEN);
}

static esp_err_t store_password(const char *pw)
{
    uint8_t salt[SALT_LEN], hash[HASH_LEN], blob[SALT_LEN + HASH_LEN];
    esp_fill_random(salt, SALT_LEN);
    hash_password(salt, pw, hash);
    memcpy(blob, salt, SALT_LEN);
    memcpy(blob + SALT_LEN, hash, HASH_LEN);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        memcpy(s_salt, salt, SALT_LEN);
        memcpy(s_hash, hash, HASH_LEN);
        s_default = false;
    }
    return err;
}

esp_err_t setup_portal_reset_password(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---------- sessions (caller holds s_lock) ---------- */

static session_t *session_find(const char *id, int64_t now)
{
    if (strlen(id) != 32) {
        return NULL;
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &s_sessions[i];
        if (!s->id[0]) {
            continue;
        }
        if (now - s->last_us > SESSION_IDLE_US) {
            s->id[0] = '\0';
            continue;
        }
        if (ct_equal((const uint8_t *)s->id, (const uint8_t *)id, 32)) {
            return s;
        }
    }
    return NULL;
}

static session_t *session_new(int64_t now)
{
    session_t *slot = &s_sessions[0];
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &s_sessions[i];
        if (!s->id[0] || now - s->last_us > SESSION_IDLE_US) {
            slot = s;
            break;
        }
        if (s->last_us < slot->last_us) {
            slot = s;               /* all busy: replace the least recently used */
        }
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        sprintf(slot->id + 2 * i, "%02x", rnd[i]);
    }
    slot->last_us = now;
    return slot;
}

static bool read_sid(httpd_req_t *req, char *sid, size_t cap)
{
    size_t len = cap;
    return httpd_req_get_cookie_val(req, "sid", sid, &len) == ESP_OK;
}

bool portal_auth_session_valid(httpd_req_t *req)
{
    char sid[40];
    if (!read_sid(req, sid, sizeof(sid))) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    session_t *s = session_find(sid, now);
    if (s) {
        s->last_us = now;
    }
    xSemaphoreGive(s_lock);
    return s != NULL;
}

esp_err_t portal_auth_reject(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    return setup_portal_send_json(req, "{\"ok\":false,\"auth\":false,\"key\":\"err.loginRequired\",\"message\":\"Login required\"}");
}

/* ---------- handlers ---------- */

esp_err_t portal_auth_login_post(httpd_req_t *req)
{
    char form[128], pw[PW_MAX + 1];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "password", pw, sizeof(pw)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.malformedRequest", "Malformed request");
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (now < s_locked_until_us) {
        xSemaphoreGive(s_lock);
        memset(pw, 0, sizeof(pw));
        return setup_portal_send_error_key(req, "err.tooManyAttemptsWait30", "Too many attempts. Wait 30 seconds.");
    }
    bool ok = password_ok(pw);
    memset(pw, 0, sizeof(pw));
    memset(form, 0, sizeof(form));
    if (!ok) {
        if (++s_fails >= MAX_FAILS) {
            s_fails = 0;
            s_locked_until_us = now + LOCKOUT_US;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "wrong password");
        httpd_resp_set_status(req, "401 Unauthorized");
        return setup_portal_send_json(req, "{\"ok\":false,\"auth\":false,\"key\":\"err.wrongPassword\",\"message\":\"Wrong password\"}");
    }
    s_fails = 0;
    session_t *s = session_new(now);
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400", s->id);
    xSemaphoreGive(s_lock);

    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return setup_portal_send_json(req, s_default ? "{\"ok\":true,\"defaultPassword\":true}"
                                                 : "{\"ok\":true,\"defaultPassword\":false}");
}

esp_err_t portal_auth_logout_post(httpd_req_t *req)
{
    char sid[40];
    if (read_sid(req, sid, sizeof(sid))) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        session_t *s = session_find(sid, esp_timer_get_time());
        if (s) {
            s->id[0] = '\0';
        }
        xSemaphoreGive(s_lock);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return setup_portal_send_json(req, "{\"ok\":true}");
}

esp_err_t portal_auth_password_post(httpd_req_t *req)
{
    char form[256], old_pw[PW_MAX + 1], new_pw[PW_MAX + 1];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "old", old_pw, sizeof(old_pw)) != ESP_OK ||
        setup_portal_form_value(form, "new", new_pw, sizeof(new_pw)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.passwordTooLongOrMalformed", "Password too long or malformed request");
    }
    portal_msg_t message = { 0 };
    size_t n = strlen(new_pw);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!password_ok(old_pw)) {
        message = PORTAL_MSG("err.theCurrentPasswordIsWrong", "The current password is wrong");
    } else if (n < PW_MIN || n > PW_MAX) {
        message = PORTAL_MSG("err.theNewPasswordMustBe", "The new password must be 8–63 characters");
    } else if (strcmp(new_pw, PORTAL_DEFAULT_PASSWORD) == 0) {
        message = PORTAL_MSG("err.chooseAPasswordOtherThan", "Choose a password other than the default one");
    } else if (store_password(new_pw) != ESP_OK) {
        message = PORTAL_MSG("err.couldNotSaveThePassword", "Could not save the password");
    } else {
        /* Everyone else logs in again; this session stays. */
        char sid[40] = "";
        read_sid(req, sid, sizeof(sid));
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (strcmp(s_sessions[i].id, sid) != 0) {
                s_sessions[i].id[0] = '\0';
            }
        }
    }
    xSemaphoreGive(s_lock);
    memset(old_pw, 0, sizeof(old_pw));
    memset(new_pw, 0, sizeof(new_pw));
    memset(form, 0, sizeof(form));
    if (message.key) {
        return setup_portal_send_msg(req, message);
    }
    ESP_LOGI(TAG, "web password changed");
    return setup_portal_send_json(req, "{\"ok\":true}");
}
