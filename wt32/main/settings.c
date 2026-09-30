/* Persistent settings of the WT32 app. */
#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NVS_NS "wt32"

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t len)
{
    char tmp[65];
    size_t n = sizeof(tmp);
    if (nvs_get_str(h, key, tmp, &n) == ESP_OK) {
        strlcpy(dst, tmp, len);
    }
}

void settings_load(wt32_settings_t *s)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    memset(s, 0, sizeof(*s));
    s->mode = WT32_MODE_CLIENT;
    snprintf(s->own_ssid, sizeof(s->own_ssid), "WT32-%02X%02X", mac[4], mac[5]);
    s->own_channel = 6;
    strlcpy(s->own_ip, CONFIG_WT32_OWN_IP, sizeof(s->own_ip));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v;
    if (nvs_get_u8(h, "mode", &v) == ESP_OK && v <= WT32_MODE_AP) {
        s->mode = v;
    }
    if (nvs_get_u8(h, "own_ch", &v) == ESP_OK && v >= 1 && v <= 13) {
        s->own_channel = v;
    }
    get_str(h, "own_ssid", s->own_ssid, sizeof(s->own_ssid));
    get_str(h, "own_pass", s->own_pass, sizeof(s->own_pass));
    get_str(h, "own_ip", s->own_ip, sizeof(s->own_ip));
    nvs_close(h);
}

bool settings_ip_ok(const char *ip)
{
    ip4_addr_t a;
    if (!ip4addr_aton(ip, &a)) {
        return false;
    }
    uint8_t last = ip4_addr4(&a);
    /* .100-.200 is the DHCP pool, .0/.255 are not hosts */
    return last >= 1 && last <= 99 && !ip4_addr_ismulticast(&a) && !ip4_addr_isloopback(&a);
}

bool settings_own_valid(const wt32_settings_t *s)
{
    size_t p = strlen(s->own_pass);
    return s->own_ssid[0] && p >= 8 && p <= 63 && settings_ip_ok(s->own_ip);
}

esp_err_t settings_save_mode(wt32_mode_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mode", mode);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_save_own(const char *ssid, const char *pass, uint8_t channel, const char *ip)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "own_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "own_pass", pass);
    if (err == ESP_OK) err = nvs_set_u8(h, "own_ch", channel);
    if (err == ESP_OK) err = nvs_set_str(h, "own_ip", ip);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
