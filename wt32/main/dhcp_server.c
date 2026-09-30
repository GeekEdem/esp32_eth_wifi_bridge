/* DHCP server task: UDP 67 on every interface (the bridge is the only one
 * with an address), replies to 68. With persist, leases are saved to NVS at
 * most every SAVE_EVERY_S seconds, reservations immediately. */
#include "dhcp_server.h"

#include <string.h>

#include "dhcp_core.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "setup_portal.h"

#define NVS_NS       "wt32"
#define NVS_KEY      "dhcp"
#define SAVE_EVERY_S 30
#define SAVE_MAX     (5 + DHCP_MAX_LEASES * 39)

static const char *TAG = "dhcps";

static dhcp_core_t s_core;
static SemaphoreHandle_t s_lock;
static int64_t s_last_save_us;
static bool s_persist;
static volatile bool s_enabled;
static bool (*s_accept)(const uint8_t mac[6]);

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000) + 1;     /* never 0 */
}

static void save(bool force)
{
    int64_t t = esp_timer_get_time();
    if (!s_persist || !s_core.dirty || (!force && t - s_last_save_us < (int64_t)SAVE_EVERY_S * 1000000)) {
        return;
    }
    static uint8_t buf[SAVE_MAX];
    size_t len = dhcp_core_save(&s_core, buf, sizeof(buf), now_s());
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK) {
            s_core.dirty = false;
            s_last_save_us = t;
        }
        nvs_close(h);
    }
}

static void load(void)
{
    static uint8_t buf[SAVE_MAX];
    size_t len = sizeof(buf);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK && !dhcp_core_load(&s_core, buf, len, now_s())) {
        ESP_LOGW(TAG, "saved leases unreadable, starting empty");
    }
    nvs_close(h);
}

static void server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(67),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "cannot bind UDP 67");
        vTaskDelete(NULL);
        return;
    }
    static uint8_t req[600], resp[576];
    while (true) {
        int n = recv(sock, req, sizeof(req), 0);
        if (n >= 34 && s_enabled && (!s_accept || s_accept(req + 28))) {    /* chaddr at 28 */
            size_t rlen = 0;
            uint32_t dst = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            dhcp_reply_t r = dhcp_core_handle(&s_core, req, n, resp, &rlen, &dst, now_s());
            xSemaphoreGive(s_lock);
            if (r != DHCP_REPLY_NONE) {
                struct sockaddr_in to = {
                    .sin_family = AF_INET,
                    .sin_port = htons(68),
                    .sin_addr.s_addr = dst,
                };
                sendto(sock, resp, rlen, 0, (struct sockaddr *)&to, sizeof(to));
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        save(false);
        xSemaphoreGive(s_lock);
    }
}

esp_err_t dhcp_server_start(const dhcp_server_config_t *cfg)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    dhcp_core_init(&s_core, cfg->server_ip, cfg->netmask, cfg->pool_first, cfg->pool_last, cfg->lease_s);
    /* No internet here: the WT32 is the router only so clients have a
     * gateway on the link; no DNS is advertised. */
    s_core.router = cfg->server_ip;
    s_persist = cfg->persist;
    s_enabled = cfg->enabled;
    s_accept = cfg->accept;
    if (s_persist) {
        load();
    }
    return xTaskCreate(server_task, "dhcps", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void dhcp_server_enable(bool on)
{
    s_enabled = on;
}

uint32_t dhcp_server_ip_of(const uint8_t mac[6])
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t ip = dhcp_core_ip_of(&s_core, mac, now_s());
    xSemaphoreGive(s_lock);
    return ip;
}

bool dhcp_server_reserve(const uint8_t mac[6], uint32_t ip)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = dhcp_core_reserve(&s_core, mac, ip);
    save(true);
    xSemaphoreGive(s_lock);
    return ok;
}

bool dhcp_server_unreserve(const uint8_t mac[6])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = dhcp_core_unreserve(&s_core, mac);
    save(true);
    xSemaphoreGive(s_lock);
    return ok;
}

size_t dhcp_server_status_json(char *buf, size_t pos, size_t cap)
{
    pos = setup_portal_appendf(buf, pos, cap, ",\"dhcp\":[");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t now = now_s();
    bool first = true;
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        const dhcp_lease_t *l = &s_core.leases[i];
        bool live = l->reserved || (l->bound && (int32_t)(l->expires - now) > 0);
        static const uint8_t zero[6];
        if (!l->used || !live || memcmp(l->mac, zero, 6) == 0) {
            continue;
        }
        char ip[16];
        esp_ip4_addr_t a = { .addr = l->ip };
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        pos = setup_portal_appendf(buf, pos, cap, "%s{\"mac\":\"" MACSTR "\",\"ip\":\"%s\",\"reserved\":%s,\"left\":%ld,\"host\":",
                                   first ? "" : ",", MAC2STR(l->mac), ip, l->reserved ? "true" : "false",
                                   l->reserved ? -1L : (long)(l->expires - now));
        pos = setup_portal_json_str(buf, pos, cap, l->host);
        pos = setup_portal_appendf(buf, pos, cap, "}");
        first = false;
    }
    xSemaphoreGive(s_lock);
    return setup_portal_appendf(buf, pos, cap, "]");
}
