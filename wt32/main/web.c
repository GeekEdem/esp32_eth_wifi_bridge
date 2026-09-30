/* WT32 web page. On top of the shared portal routes:
 *   POST /api/mode      mode=client|own|ap [restart=0]
 *   POST /api/own       ssid, pass, channel, ip   (access point settings, both AP modes)
 *   POST /api/relearn   forget the learned device (client mode)
 *   POST /api/dhcp/reserve    mac, ip   (own network)
 *   POST /api/dhcp/unreserve  mac
 * /api/status gets "mode" and the fields of that mode.
 * Client mode serves on port 80 (setup AP) and on the management port
 * (home network, shared IP); the router and access point modes on port 80
 * of the WT32's address. */
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_mode.h"
#include "dhcp_server.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "own_mode.h"
#include "sdkconfig.h"
#include "setup_portal.h"

extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");

static wt32_settings_t s_set;
static bool s_setup_boot;

static void ip_str(uint32_t addr, char *buf, size_t len)
{
    buf[0] = '\0';
    if (addr) {
        ip4_addr_t a = { .addr = addr };
        ip4addr_ntoa_r(&a, buf, len);
    }
}

static size_t own_settings_json(char *buf, size_t pos, size_t cap)
{
    pos = setup_portal_appendf(buf, pos, cap, ",\"own\":{\"ssid\":");
    pos = setup_portal_json_str(buf, pos, cap, s_set.own_ssid);
    return setup_portal_appendf(buf, pos, cap, ",\"channel\":%d,\"ip\":\"%s\",\"passSet\":%s}",
                                s_set.own_channel, s_set.own_ip, strlen(s_set.own_pass) >= 8 ? "true" : "false");
}

size_t web_status_extra(char *buf, size_t pos, size_t cap)
{
    char mac[18] = "", ip[16], mgmt[16];
    if (s_setup_boot) {
        pos = setup_portal_appendf(buf, pos, cap, ",\"setupBoot\":true");
    }
    if (s_set.mode == WT32_MODE_AP) {
        static const char *const UPLINK[] = { "", "waiting", "dhcp", "fallback" };
        own_stats_t s;
        own_mode_get_stats(&s);
        ip_str(s.ip, mgmt, sizeof(mgmt));
        ip_str(s.gw, ip, sizeof(ip));
        pos = setup_portal_appendf(buf, pos, cap,
            ",\"mode\":\"ap\",\"eth\":%s,\"apClients\":%d,\"devMac\":\"\",\"devIp\":\"\","
            "\"apIp\":\"%s\",\"uplink\":\"%s\",\"gw\":\"%s\"",
            s.eth_up ? "true" : "false", s.ap_clients, mgmt, UPLINK[s.uplink], ip);
        return own_settings_json(buf, pos, cap);
    }
    if (s_set.mode == WT32_MODE_OWN) {
        own_stats_t s;
        own_mode_get_stats(&s);
        if (s.dev_known) {
            snprintf(mac, sizeof(mac), MACSTR, MAC2STR(s.dev_mac));
        }
        ip_str(s.dev_ip, ip, sizeof(ip));
        ip_str(s.ip, mgmt, sizeof(mgmt));
        pos = setup_portal_appendf(buf, pos, cap,
            ",\"mode\":\"own\",\"eth\":%s,\"apClients\":%d,\"devMac\":\"%s\",\"devIp\":\"%s\",\"ownIp\":\"%s\"",
            s.eth_up ? "true" : "false", s.ap_clients, mac, ip, mgmt);
        pos = dhcp_server_status_json(buf, pos, cap);
        return own_settings_json(buf, pos, cap);
    }
    client_stats_t s;
    client_mode_get_stats(&s);
    if (s.dev_known) {
        snprintf(mac, sizeof(mac), MACSTR, MAC2STR(s.dev_mac));
    }
    ip_str(s.dev_ip, ip, sizeof(ip));
    ip_str(s.mgmt_ip, mgmt, sizeof(mgmt));
    pos = setup_portal_appendf(buf, pos, cap,
        ",\"mode\":\"client\",\"wifiUp\":%s,\"eth\":%s,\"devMac\":\"%s\",\"devIp\":\"%s\","
        "\"toWifi\":[%lu,%lu],\"toEth\":[%lu,%lu],"
        "\"dropWifiDown\":%lu,\"dropEthDown\":%lu,\"txErrWifi\":%lu,\"txErrEth\":%lu,"
        "\"foreign\":%lu,\"ipv6Dropped\":%lu,\"dhcpRewrites\":%lu,"
        "\"mgmtIp\":\"%s\",\"mgmtPort\":%d,\"mgmtFrames\":[%lu,%lu],\"mgmtTxErr\":%lu,"
        "\"mgmtFlows\":%d,\"mgmtEvictions\":%lu,\"mgmtReach\":%s,\"devLease\":%s",
        s.wifi_up ? "true" : "false", s.eth_up ? "true" : "false", mac, ip,
        (unsigned long)s.to_wifi_frames, (unsigned long)s.to_wifi_bytes,
        (unsigned long)s.to_eth_frames, (unsigned long)s.to_eth_bytes,
        (unsigned long)s.drop_wifi_down, (unsigned long)s.drop_eth_down,
        (unsigned long)s.tx_err_wifi, (unsigned long)s.tx_err_eth,
        (unsigned long)s.foreign_frames, (unsigned long)s.ipv6_dropped,
        (unsigned long)s.dhcp_rewrites,
        mgmt, CONFIG_WT32_MGMT_PORT, (unsigned long)s.mgmt_rx_frames, (unsigned long)s.mgmt_tx_frames,
        (unsigned long)s.mgmt_tx_err, s.mgmt_flows, (unsigned long)s.mgmt_evictions,
        s.mgmt_reachable ? "true" : "false", s.dev_ip_leased ? "true" : "false");
    return own_settings_json(buf, pos, cap);
}

static esp_err_t mode_post(httpd_req_t *req)
{
    char form[64], mode[16], restart[4];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "mode", mode, sizeof(mode)) != ESP_OK ||
        setup_portal_form_value(form, "restart", restart, sizeof(restart)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.malformedRequest", "Malformed request");
    }
    wt32_mode_t m;
    if (strcmp(mode, "client") == 0) {
        m = WT32_MODE_CLIENT;
    } else if (strcmp(mode, "own") == 0 || strcmp(mode, "ap") == 0) {
        wt32_settings_t cur;
        settings_load(&cur);
        if (!settings_own_valid(&cur)) {
            return setup_portal_send_error_key(req, "err.saveTheWt32NetworkSettings", "Save the WT32 network settings first (password of 8–63 characters)");
        }
        m = mode[0] == 'o' ? WT32_MODE_OWN : WT32_MODE_AP;
    } else {
        return setup_portal_send_error_key(req, "err.unknownMode", "Unknown mode");
    }
    if (settings_save_mode(m) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.couldNotSaveTheMode", "Could not save the mode");
    }
    if (strcmp(restart, "0") != 0) {
        setup_portal_restart_later();
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t own_post(httpd_req_t *req)
{
    char form[256], ssid[33], pass[65], ch[4], ip[16];
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        setup_portal_form_value(form, "pass", pass, sizeof(pass)) != ESP_OK ||
        setup_portal_form_value(form, "channel", ch, sizeof(ch)) != ESP_OK ||
        setup_portal_form_value(form, "ip", ip, sizeof(ip)) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.formDataTooLongOr", "Form data too long or malformed");
    }
    wt32_settings_t cur;
    settings_load(&cur);
    if (!pass[0]) {
        strlcpy(pass, cur.own_pass, sizeof(pass));     /* empty = keep the current one */
    }
    int channel = atoi(ch);
    size_t plen = strlen(pass);
    if (!ssid[0]) {
        return setup_portal_send_error_key(req, "err.enterTheNetworkName", "Enter the network name");
    }
    if (plen < 8 || plen > 63) {
        return setup_portal_send_error_key(req, "err.theNetworkPasswordMustBe", "The network password must be 8–63 characters");
    }
    if (channel < 1 || channel > 13) {
        return setup_portal_send_error_key(req, "err.channel1To13", "Channel: 1 to 13");
    }
    if (!settings_ip_ok(ip)) {
        return setup_portal_send_error_key(req, "err.wt32AddressIpv4EndingIn", "WT32 address: IPv4 ending in 1–99 (.100–.200 is the DHCP pool)");
    }
    if (settings_save_own(ssid, pass, channel, ip) != ESP_OK) {
        return setup_portal_send_error_key(req, "err.couldNotSave", "Could not save");
    }
    settings_load(&s_set);
    memset(pass, 0, sizeof(pass));
    memset(form, 0, sizeof(form));
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) {
            return false;
        }
        mac[i] = v[i];
    }
    return true;
}

static esp_err_t reserve_post(httpd_req_t *req)
{
    if (s_set.mode != WT32_MODE_OWN) {
        return setup_portal_send_error_key(req, "err.addressPinningWorksInRouter", "Address pinning works in Router mode only");
    }
    char form[96], mac_s[24], ip_s[16];
    uint8_t mac[6];
    ip4_addr_t ip;
    if (setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "mac", mac_s, sizeof(mac_s)) != ESP_OK ||
        setup_portal_form_value(form, "ip", ip_s, sizeof(ip_s)) != ESP_OK ||
        !parse_mac(mac_s, mac) || !ip4addr_aton(ip_s, &ip)) {
        return setup_portal_send_error_key(req, "err.invalidMacOrIpAddress", "Invalid MAC or IP address");
    }
    if (!dhcp_server_reserve(mac, ip.addr)) {
        return setup_portal_send_error_key(req, "err.theAddressIsOutsideThe", "The address is outside the WT32 network, is the WT32's own, or is pinned to another device");
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t unreserve_post(httpd_req_t *req)
{
    char form[64], mac_s[24];
    uint8_t mac[6];
    if (s_set.mode != WT32_MODE_OWN ||
        setup_portal_read_form(req, form, sizeof(form)) != ESP_OK ||
        setup_portal_form_value(form, "mac", mac_s, sizeof(mac_s)) != ESP_OK || !parse_mac(mac_s, mac)) {
        return setup_portal_send_error_key(req, "err.invalidRequest", "Invalid request");
    }
    if (!dhcp_server_unreserve(mac)) {
        return setup_portal_send_error_key(req, "err.noAddressIsPinnedFor", "No address is pinned for this device");
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t relearn_post(httpd_req_t *req)
{
    if (s_set.mode == WT32_MODE_CLIENT) {
        client_mode_relearn();
    }
    return setup_portal_send_json(req, "{\"ok\":true}");
}

esp_err_t web_start(const wt32_settings_t *s, bool setup_boot)
{
    s_set = *s;
    s_setup_boot = setup_boot;
    const setup_portal_config_t cfg = {
        .page = page_start,
        .page_len = page_end - page_start - 1,   /* EMBED_TXTFILES adds a NUL */
        .status_extra = web_status_extra,
        .ota = true,
        .langs = portal_langs,
        .lang_count = portal_lang_count,
        .lan_port = s->mode == WT32_MODE_CLIENT ? CONFIG_WT32_MGMT_PORT : 0,
    };
    esp_err_t err = setup_portal_start(&cfg);
    const httpd_uri_t uris[] = {
        { .uri = "/api/mode",    .method = HTTP_POST, .handler = mode_post },
        { .uri = "/api/own",     .method = HTTP_POST, .handler = own_post },
        { .uri = "/api/relearn", .method = HTTP_POST, .handler = relearn_post },
        { .uri = "/api/dhcp/reserve",   .method = HTTP_POST, .handler = reserve_post },
        { .uri = "/api/dhcp/unreserve", .method = HTTP_POST, .handler = unreserve_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]) && err == ESP_OK; i++) {
        err = setup_portal_register(&uris[i]);
    }
    return err;
}
