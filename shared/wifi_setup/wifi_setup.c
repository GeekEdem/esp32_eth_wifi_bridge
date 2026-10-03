/* Wi-Fi station / setup access point management. */
#include "wifi_setup.h"

#include <stdio.h>
#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "setup_portal.h"

#define NVS_NS            "wifi_setup"
#define FALLBACK_US       (60LL * 1000 * 1000)   /* setup AP after 60 s offline */
#define RETRY_US          (2LL * 1000 * 1000)
#define RETRY_AP_BUSY_US  (20LL * 1000 * 1000)   /* someone is in the portal */

static const char *TAG = "wifi";

static char s_ssid[33];
static char s_pass[65];
static char s_host[33];
static char s_ap_ssid[33];
static wifi_setup_config_t s_cfg;

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_fallback_timer;
static volatile wifi_setup_state_t s_state;
static volatile bool s_ap_active;
static volatile bool s_ap_requested;     /* by the application, see wifi_setup_request_ap() */
static volatile bool s_started;          /* read by other tasks, see wifi_setup_started() */

static void nvs_read_str(nvs_handle_t h, const char *key, char *dst, size_t len)
{
    char tmp[65];
    size_t n = sizeof(tmp);
    if (nvs_get_str(h, key, tmp, &n) == ESP_OK) {
        strlcpy(dst, tmp, len);
    }
}

static void load_settings(void)
{
    strlcpy(s_ssid, s_cfg.default_ssid ? s_cfg.default_ssid : "", sizeof(s_ssid));
    strlcpy(s_pass, s_cfg.default_password ? s_cfg.default_password : "", sizeof(s_pass));
    strlcpy(s_host, s_cfg.default_hostname ? s_cfg.default_hostname : "esp32", sizeof(s_host));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_read_str(h, "ssid", s_ssid, sizeof(s_ssid));
        nvs_read_str(h, "pass", s_pass, sizeof(s_pass));
        nvs_read_str(h, "host", s_host, sizeof(s_host));
        nvs_close(h);
    }
}

static bool ap_has_clients(void)
{
    wifi_sta_list_t list;
    return s_ap_active && esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}

static void ap_enable(void)
{
    if (s_ap_active) {
        return;
    }
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    setup_portal_dns_start(ip.ip.addr);
    s_ap_active = true;
    ESP_LOGI(TAG, "setup AP \"%s\" at " IPSTR, s_ap_ssid, IP2STR(&ip.ip));
}

static void ap_disable(void)
{
    if (!s_ap_active || s_cfg.ap_always_on || s_ap_requested) {
        return;
    }
    setup_portal_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_active = false;
    ESP_LOGI(TAG, "setup AP off");
}

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void fallback_cb(void *arg)
{
    if (s_state != WIFI_SETUP_CONNECTED) {
        ESP_LOGW(TAG, "\"%s\" unreachable, starting setup AP", s_ssid);
        ap_enable();
    }
}

static void on_connected(void)
{
    s_state = WIFI_SETUP_CONNECTED;
    esp_timer_stop(s_fallback_timer);
    ap_disable();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated with \"%s\"", s_ssid);
        if (!s_cfg.sta_netif) {
            on_connected();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        if (s_state == WIFI_SETUP_CONNECTED) {
            ESP_LOGW(TAG, "lost \"%s\" (reason %d)", s_ssid, ev->reason);
            esp_timer_start_once(s_fallback_timer, FALLBACK_US);
        }
        if (s_ssid[0]) {
            s_state = WIFI_SETUP_CONNECTING;
            /* A connection attempt makes the AP hop channels; go easy while
             * someone is filling in the portal. */
            esp_timer_start_once(s_retry_timer, ap_has_clients() ? RETRY_AP_BUSY_US : RETRY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR ", http://%s.local", IP2STR(&ev->ip_info.ip), s_host);
        on_connected();
    }
}

/* Hand out the AP's own address as DNS, so the captive portal DNS catches
 * every lookup from setup clients. */
static void ap_dhcp_offer_dns(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    esp_netif_dns_info_t dns = {
        .ip.type = ESP_IPADDR_TYPE_V4,
        .ip.u_addr.ip4.addr = ip.ip.addr,
    };
    uint8_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(s_ap);
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_start(s_ap);
}

esp_err_t wifi_setup_start(const wifi_setup_config_t *cfg)
{
    s_cfg = *cfg;
    load_settings();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", s_cfg.ap_prefix, mac[4], mac[5]);

    if (s_cfg.sta_netif) {
        s_sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(s_sta, s_host);
    }
    s_ap = esp_netif_create_default_wifi_ap();
    ap_dhcp_offer_dns();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    /* Credentials live in our own NVS namespace. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "evt");

    const esp_timer_create_args_t retry = { .callback = retry_cb, .name = "wifi_retry" };
    const esp_timer_create_args_t fallback = { .callback = fallback_cb, .name = "wifi_fallback" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry, &s_retry_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&fallback, &s_fallback_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    if (s_ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta config");
        s_state = WIFI_SETUP_CONNECTING;
    } else {
        s_state = WIFI_SETUP_UNCONFIGURED;
    }
    if (!s_ssid[0] || s_cfg.ap_always_on || s_ap_requested) {
        ap_enable();
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    /* Power save adds tens of ms per round trip; esptool is round-trip bound. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (s_cfg.tx_power > 0) {
        esp_wifi_set_max_tx_power(s_cfg.tx_power);
    }
    if (s_ssid[0]) {
        esp_timer_start_once(s_fallback_timer, FALLBACK_US);
    }

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns");
    mdns_hostname_set(s_host);
    mdns_instance_name_set(s_cfg.mdns_instance ? s_cfg.mdns_instance : s_host);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_started = true;
    return ESP_OK;
}

void wifi_setup_request_ap(bool on)
{
    if (s_ap_requested == on) {
        return;
    }
    s_ap_requested = on;
    if (!s_started) {
        return;                         /* applied by wifi_setup_start() */
    }
    if (on) {
        ap_enable();
    } else if (s_state == WIFI_SETUP_CONNECTED) {
        ap_disable();
    }
}

bool wifi_setup_started(void)
{
    return s_started;
}

wifi_setup_state_t wifi_setup_state(void)
{
    return s_state;
}

bool wifi_setup_ap_active(void)
{
    return s_ap_active;
}

const char *wifi_setup_ssid(void)
{
    return s_ssid;
}

const char *wifi_setup_hostname(void)
{
    return s_host;
}

const char *wifi_setup_ap_ssid(void)
{
    return s_ap_ssid;
}

void wifi_setup_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (s_sta && s_state == WIFI_SETUP_CONNECTED && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    } else {
        buf[0] = '\0';
    }
}

int wifi_setup_rssi(void)
{
    wifi_ap_record_t ap;
    return (s_state == WIFI_SETUP_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

int wifi_setup_scan(wifi_ap_record_t *out, int max)
{
    const wifi_scan_config_t cfg = {0};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        return 0;   /* e.g. the station is busy connecting */
    }
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) {
        return 0;
    }
    return n;
}

esp_err_t wifi_setup_save(const char *ssid, const char *password, const char *hostname)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", password);
    }
    if (err == ESP_OK && hostname && hostname[0]) {
        err = nvs_set_str(h, "host", hostname);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t wifi_setup_forget(void)
{
    /* An empty SSID (rather than a missing key) also overrides a network
     * baked in through menuconfig. */
    return wifi_setup_save("", "", NULL);
}
