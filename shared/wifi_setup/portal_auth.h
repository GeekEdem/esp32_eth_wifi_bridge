/* Web page password and sessions (internal to setup_portal).
 *
 * - One password for the page; default "12345678" until changed.
 * - Stored as salt + SHA-256(salt || password) in NVS.
 * - Login sets an HttpOnly cookie; sessions expire after 30 min idle.
 * - 5 wrong attempts in a row lock logins for 30 s.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define PORTAL_DEFAULT_PASSWORD "12345678"

esp_err_t portal_auth_init(void);

bool portal_auth_session_valid(httpd_req_t *req);
bool portal_auth_is_default(void);

/* Route handlers (public: login; the rest require a session). */
esp_err_t portal_auth_login_post(httpd_req_t *req);
esp_err_t portal_auth_logout_post(httpd_req_t *req);
esp_err_t portal_auth_password_post(httpd_req_t *req);

/* 401 with a JSON body the page script understands. */
esp_err_t portal_auth_reject(httpd_req_t *req);
