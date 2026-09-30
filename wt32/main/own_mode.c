/* The two access-point modes. In both the WT32 is an access point, and the
 * AP and Ethernet are one L2 network (lwIP bridge).
 *
 * Router (WT32_MODE_OWN): the WT32 has a static address and runs the DHCP
 * server (dhcp_server.c: leases survive restarts, addresses can be reserved
 * per MAC). The Ethernet device and Wi-Fi clients get addresses from the
 * same pool and reach each other directly, like behind a home router but
 * without internet.
 *
 *   WT32   <own_ip>          (/24, web page on port 80)
 *   pool   .100 - .200
 *
 * Access point (WT32_MODE_AP): the cable goes to a router (or a switch of a
 * network with DHCP); Wi-Fi clients get everything from it, and the WT32 is
 * one more DHCP client there (page at its address or <hostname>.local). If
 * no address comes within FALLBACK_AFTER_S, the WT32 takes <own_ip>/24 and
 * serves short leases from .100-.200 to its Wi-Fi clients only (never to the
 * cable), so the page stays reachable; meanwhile it probes the cable for a
 * DHCP server every PROBE_EVERY_S and on link-up, and goes back to DHCP as
 * soon as one answers.
 */
#include "own_mode.h"

#include <string.h>

#include "dhcp_server.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_br_glue.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "dhcp_core.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"

#define POOL_FIRST 100
#define POOL_LAST  200
#define LEASE_S    (2 * 3600)
#define FALLBACK_LEASE_S  120       /* short: clients move to the router soon after it appears */
#define FALLBACK_AFTER_S  30
#define PROBE_EVERY_S     30

static const char *TAG = "own";

static esp_netif_t *s_eth_port;
static esp_netif_t *s_br;
static volatile bool s_eth_up;
static volatile int s_ap_clients;
static volatile bool s_dev_known;
static uint8_t s_dev_mac[6];
static uint8_t s_eth_mac[6];
static esp_netif_ip_info_t s_ip;
static char s_ssid[33];
static bool s_ap_mode;
static volatile uplink_t s_uplink;

/* Ethernet -> bridge port, noting the device's MAC on the way (the glue's own
 * input function does only the esp_netif_receive()). */
static esp_err_t eth_input(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
    if (!s_dev_known && len >= 14 && !(buf[6] & 0x01) && memcmp(buf + 6, s_eth_mac, 6) != 0) {
        memcpy(s_dev_mac, buf + 6, 6);
        s_dev_known = true;
        ESP_LOGI(TAG, "device MAC " MACSTR, MAC2STR(s_dev_mac));
    }
    return esp_netif_receive((esp_netif_t *)priv, buf, len, NULL);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
        s_eth_up = true;
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Ethernet link down");
        s_eth_up = false;
        s_dev_known = false;            /* a different device may come next */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
    }
}

/* Access point: the fallback DHCP server answers Wi-Fi clients only. */
static bool is_ap_station(const uint8_t mac[6])
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < list.num; i++) {
        if (memcmp(list.sta[i].mac, mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void mdns_readdress(void)
{
    mdns_netif_action(s_br, MDNS_EVENT_DISABLE_IP4);
    mdns_netif_action(s_br, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
}

/* One DISCOVER to the cable's network; true if a DHCP server offers an
 * address. The DHCP client is stopped meanwhile, so port 68 is free. */
static bool probe_dhcp(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct ifreq ifr = { 0 };
    esp_netif_get_netif_impl_name(s_br, ifr.ifr_name);
    setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr));
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(68), .sin_addr.s_addr = htonl(INADDR_ANY) };
    bool found = false;
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) == 0) {
        static uint8_t buf[600];
        uint32_t xid = esp_random();
        size_t len = dhcp_probe_build(buf, sizeof(buf), s_eth_mac, xid);
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
        sendto(sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
        for (int i = 0; i < 3 && !found; i++) {         /* ~3 s for the answer */
            int n = recv(sock, buf, sizeof(buf), 0);
            uint32_t server = 0;
            if (n > 0 && dhcp_probe_is_offer(buf, n, s_eth_mac, xid, s_ip.ip.addr, &server)) {
                esp_ip4_addr_t a = { .addr = server };
                ESP_LOGI(TAG, "DHCP server " IPSTR " on the cable", IP2STR(&a));
                found = true;
            }
        }
    }
    close(sock);
    return found;
}

/* Access point: DHCP on the cable, or the fallback address while there is none. */
static void uplink_task(void *arg)
{
    int no_ip_s = 0, probe_in = 0;
    bool eth_was_up = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool eth = s_eth_up;
        bool link_came = eth && !eth_was_up;
        eth_was_up = eth;

        if (s_uplink != UPLINK_FALLBACK) {
            esp_netif_ip_info_t ip = { 0 };
            esp_netif_get_ip_info(s_br, &ip);
            if (ip.ip.addr) {
                if (s_uplink != UPLINK_DHCP) {
                    s_uplink = UPLINK_DHCP;
                    ESP_LOGI(TAG, "address from the cable's DHCP: http://" IPSTR, IP2STR(&ip.ip));
                    mdns_readdress();
                }
                no_ip_s = 0;
                continue;
            }
            s_uplink = UPLINK_WAITING;
            if (++no_ip_s < FALLBACK_AFTER_S) {
                continue;
            }
            esp_netif_dhcpc_stop(s_br);
            esp_netif_set_ip_info(s_br, &s_ip);
            dhcp_server_enable(true);
            s_uplink = UPLINK_FALLBACK;
            probe_in = PROBE_EVERY_S;
            ESP_LOGW(TAG, "no DHCP on the cable for %d s: fallback http://" IPSTR ", leases to Wi-Fi clients",
                     FALLBACK_AFTER_S, IP2STR(&s_ip.ip));
            mdns_readdress();
            continue;
        }
        if (!eth || (!link_came && --probe_in > 0)) {
            continue;
        }
        probe_in = PROBE_EVERY_S;
        if (probe_dhcp()) {
            dhcp_server_enable(false);
            esp_netif_dhcpc_start(s_br);
            s_uplink = UPLINK_WAITING;
            no_ip_s = 0;
        }
    }
}

esp_err_t own_mode_start(esp_eth_handle_t eth, const wt32_settings_t *s)
{
    strlcpy(s_ssid, s->own_ssid, sizeof(s_ssid));
    s_ap_mode = s->mode == WT32_MODE_AP;
    esp_read_mac(s_eth_mac, ESP_MAC_ETH);

    /* Ethernet bridge port: no IP of its own */
    esp_netif_inherent_config_t eth_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    eth_cfg.flags = 0;
    eth_cfg.if_key = "ETH_PORT";
    eth_cfg.if_desc = "eth port";
    const esp_netif_config_t eth_netif_cfg = { .base = &eth_cfg, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s_eth_port = esp_netif_new(&eth_netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_port, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_eth_update_input_path(eth, eth_input, s_eth_port));

    /* Wi-Fi: AP for the own network; STA stays idle, only so the page can scan
     * when switching back to the client mode. */
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_netif_inherent_config_t ap_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    ap_cfg.flags = ESP_NETIF_FLAG_AUTOUP;
    ap_cfg.ip_info = NULL;
    esp_netif_t *ap_port = esp_netif_create_wifi(WIFI_IF_AP, &ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());

    wifi_config_t ap = {
        .ap = {
            .channel = s->own_channel,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .max_connection = 8,
            .pmf_cfg = { .capable = true },
        },
    };
    strlcpy((char *)ap.ap.ssid, s->own_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s->own_ssid);
    strlcpy((char *)ap.ap.password, s->own_pass, sizeof(ap.ap.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* Bridge: static address + our DHCP server (router), or a DHCP client
     * with the fallback address (access point) */
    esp_netif_inherent_config_t br_cfg = ESP_NETIF_INHERENT_DEFAULT_BR();
    if (!s_ap_mode) {
        br_cfg.flags = ESP_NETIF_FLAG_IS_BRIDGE;     /* no DHCP client: static address */
    }
    br_cfg.ip_info = NULL;
    bridgeif_config_t bridge = {
        .max_fdb_dyn_entries = 16,
        .max_fdb_sta_entries = 2,
        .max_ports = 2,
    };
    br_cfg.bridge_info = &bridge;
    memcpy(br_cfg.mac, s_eth_mac, 6);
    const esp_netif_config_t br_netif_cfg = { .base = &br_cfg, .stack = ESP_NETIF_NETSTACK_DEFAULT_BR };
    s_br = esp_netif_new(&br_netif_cfg);

    esp_netif_br_glue_handle_t glue = esp_netif_br_glue_new();
    ESP_ERROR_CHECK(esp_netif_br_glue_add_port(glue, s_eth_port));
    ESP_ERROR_CHECK(esp_netif_br_glue_add_wifi_port(glue, ap_port));
    ESP_ERROR_CHECK(esp_netif_attach(s_br, glue));
    esp_netif_set_hostname(s_br, CONFIG_WT32_HOSTNAME);

    s_ip.ip.addr = esp_ip4addr_aton(s->own_ip);
    s_ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    s_ip.gw.addr = s_ip.ip.addr;
    uint32_t net = ntohl(s_ip.ip.addr) & 0xFFFFFF00;
    dhcp_server_config_t dhcp = {
        .server_ip = s_ip.ip.addr,
        .netmask = s_ip.netmask.addr,
        .pool_first = htonl(net | POOL_FIRST),
        .pool_last = htonl(net | POOL_LAST),
        .lease_s = LEASE_S,
        .persist = true,
        .enabled = true,
    };
    if (s_ap_mode) {
        dhcp.lease_s = FALLBACK_LEASE_S;
        dhcp.persist = false;
        dhcp.enabled = false;                       /* until the fallback */
        dhcp.accept = is_ap_station;
        s_uplink = UPLINK_WAITING;
    } else {
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_br, &s_ip));
        s_uplink = UPLINK_NONE;
    }
    ESP_ERROR_CHECK(dhcp_server_start(&dhcp));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_event, NULL));

    ESP_ERROR_CHECK(eth_start_promiscuous(eth));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(CONFIG_WT32_HOSTNAME);
    mdns_instance_name_set("WT32");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (mdns_register_netif(s_br) == ESP_OK) {
        mdns_netif_action(s_br, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
    }

    if (s_ap_mode) {
        xTaskCreate(uplink_task, "uplink", 3072, NULL, 4, NULL);
        ESP_LOGI(TAG, "access point \"%s\" on channel %d for the network on the cable, "
                 "WT32 by DHCP (%s.local), fallback " IPSTR " after %d s",
                 s_ssid, s->own_channel, CONFIG_WT32_HOSTNAME, IP2STR(&s_ip.ip), FALLBACK_AFTER_S);
    } else {
        ESP_LOGI(TAG, "router \"%s\" on channel %d, WT32 at http://" IPSTR " (%s.local), pool .%d-.%d",
                 s_ssid, s->own_channel, IP2STR(&s_ip.ip), CONFIG_WT32_HOSTNAME, POOL_FIRST, POOL_LAST);
    }
    return ESP_OK;
}

void own_mode_get_stats(own_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->eth_up = s_eth_up;
    out->ap_clients = s_ap_clients;
    out->uplink = s_uplink;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    if (s_ap_mode) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_get_ip_info(s_br, &ip);
        out->ip = ip.ip.addr;
        out->gw = s_uplink == UPLINK_DHCP ? ip.gw.addr : 0;
        return;                 /* the cable is a whole network, not one device */
    }
    out->ip = s_ip.ip.addr;
    out->dev_known = s_dev_known;
    if (s_dev_known) {
        memcpy(out->dev_mac, s_dev_mac, 6);
        out->dev_ip = dhcp_server_ip_of(s_dev_mac);
    }
}
