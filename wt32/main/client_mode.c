/* Client mode: Ethernet <-> Wi-Fi station forwarding for one device.
 *
 * Ethernet frames never reach lwIP: the EMAC runs promiscuous and hands every
 * frame to eth_input(). Station frames bypass the default Wi-Fi netif through
 * the driver's RX hook; MAC rewriting lives in l2rewrite.c.
 *
 * The WT32's own traffic uses a separate netif ("MGMT") with the station MAC
 * and the device's IP: mgmt_demux.c picks the frames that belong to it (new
 * connections to the management port and flows it opened), and whatever it
 * sends goes straight out of the station.
 */
#include "client_mode.h"

#include <stdlib.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eth.h"
#include "l2rewrite.h"
#include "mdns.h"
#include "mgmt_demux.h"
#include "sdkconfig.h"
#include "wifi_setup.h"

static const char *TAG = "client";

static esp_eth_handle_t s_eth;
static l2rw_t s_rw;                         /* written by the EMAC RX and Wi-Fi tasks */
static portMUX_TYPE s_rw_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_wifi_up;
static volatile bool s_eth_up;
static volatile bool s_relearn;

static esp_netif_t *s_mgmt;
static demux_t s_demux;
static portMUX_TYPE s_demux_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_ip_info_t s_mgmt_ip;       /* as applied to the netif */
static volatile l2rw_reach_t s_mgmt_reach;  /* does the address work on the Wi-Fi network */
static volatile bool s_mgmt_from_lease;

/* Wi-Fi TX buffers full: how many ticks (10 ms) a frame from the device may
 * wait for the driver before it is dropped. */
#define WIFI_TX_WAIT_TICKS  5

static struct {
    uint32_t wifi_tx_waits;                 /* frames that had to wait for Wi-Fi */
    esp_err_t wifi_tx_last_err;
    uint32_t to_wifi_frames, to_wifi_bytes;
    uint32_t to_eth_frames, to_eth_bytes;
    uint32_t drop_wifi_down, drop_eth_down;
    uint32_t tx_err_wifi, tx_err_eth;
    uint32_t mgmt_rx_frames, mgmt_tx_frames, mgmt_tx_err;
    uint32_t mgmt_tx_waits;
    esp_err_t mgmt_tx_last_err;
} s_st;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Hands a frame from the device to the station. When the Wi-Fi driver's TX
 * buffers are full, waits for it to send some instead of dropping the frame:
 * meanwhile the EMAC's RX descriptors fill up and it sends PAUSE frames to the
 * device (flow control, eth.c), so a fast sender slows down to the Wi-Fi rate. */
static esp_err_t wifi_tx_from_device(uint8_t *buf, uint32_t len)
{
    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, buf, len);
    if (err == ESP_ERR_NO_MEM) {
        s_st.wifi_tx_waits++;
        for (int i = 0; i < WIFI_TX_WAIT_TICKS && err == ESP_ERR_NO_MEM && s_wifi_up; i++) {
            vTaskDelay(1);
            err = esp_wifi_internal_tx(WIFI_IF_STA, buf, len);
        }
    }
    if (err != ESP_OK) {
        s_st.wifi_tx_last_err = err;
    }
    return err;
}

/* Device -> Wi-Fi (EMAC RX task). */
static esp_err_t eth_input(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_rw_lock);
    if (s_relearn) {
        s_relearn = false;
        l2rw_forget(&s_rw);
    }
    bool was_known = s_rw.dev_known;
    l2rw_verdict_t v = l2rw_from_wired(&s_rw, buf, len, now);
    portEXIT_CRITICAL(&s_rw_lock);
    if (!was_known && s_rw.dev_known) {
        ESP_LOGI(TAG, "device MAC " MACSTR, MAC2STR(s_rw.dev_mac));
    }
    if (v == L2RW_FORWARD) {
        if (!s_wifi_up) {
            s_st.drop_wifi_down++;
        } else if (wifi_tx_from_device(buf, len) == ESP_OK) {
            s_st.to_wifi_frames++;
            s_st.to_wifi_bytes += len;
        } else {
            s_st.tx_err_wifi++;
        }
    }
    free(buf);
    return ESP_OK;
}

/* Wi-Fi -> WT32 and/or device (Wi-Fi task). The decision is made on the
 * frame as received, before the MACs are rewritten for the device. */
static esp_err_t wifi_input(void *buf, uint16_t len, void *eb)
{
    portENTER_CRITICAL(&s_demux_lock);
    demux_target_t target = demux_inbound(&s_demux, buf, len, now_ms());
    portEXIT_CRITICAL(&s_demux_lock);

    if (target != DEMUX_DEVICE) {
        /* lwIP keeps the buffer until it is done; the Wi-Fi buffer is ours
         * to release now, so hand over a copy. Freed by mgmt_free_rx(). */
        void *copy = malloc(len);
        if (copy) {
            memcpy(copy, buf, len);
            esp_netif_receive(s_mgmt, copy, len, copy);
            s_st.mgmt_rx_frames++;
        }
    }
    if (target != DEMUX_LOCAL) {
        l2rw_verdict_t v = L2RW_DROP;
        if (s_eth_up) {
            uint32_t now = now_ms();
            portENTER_CRITICAL(&s_rw_lock);
            v = l2rw_to_wired(&s_rw, buf, len, now);
            portEXIT_CRITICAL(&s_rw_lock);
        }
        if (!s_eth_up) {
            s_st.drop_eth_down++;
        } else if (v == L2RW_FORWARD) {
            if (esp_eth_transmit(s_eth, buf, len) == ESP_OK) {
                s_st.to_eth_frames++;
                s_st.to_eth_bytes += len;
            } else {
                s_st.tx_err_eth++;
            }
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

/* WT32 -> Wi-Fi (tcpip thread). Frames already carry the station MAC. */
static esp_err_t mgmt_transmit(void *h, void *buf, size_t len)
{
    portENTER_CRITICAL(&s_demux_lock);
    demux_outbound(&s_demux, buf, len, now_ms());
    portEXIT_CRITICAL(&s_demux_lock);
    if (!s_wifi_up) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, buf, len);
    if (err == ESP_ERR_NO_MEM) {
        /* Wi-Fi TX buffers full (the device is sending hard): one tick for the
         * driver to drain. This is the tcpip thread, so no longer than that;
         * TCP retransmits what is still lost. */
        s_st.mgmt_tx_waits++;
        vTaskDelay(1);
        err = esp_wifi_internal_tx(WIFI_IF_STA, buf, len);
    }
    if (err == ESP_OK) {
        s_st.mgmt_tx_frames++;
    } else {
        s_st.mgmt_tx_err++;
        s_st.mgmt_tx_last_err = err;
    }
    return err;
}

static void mgmt_free_rx(void *h, void *buf)
{
    free(buf);
}

static void mgmt_netif_init(const uint8_t sta_mac[6])
{
    static const esp_netif_driver_ifconfig_t driver = {
        .handle = (void *)1,                /* unused, must be non-NULL */
        .transmit = mgmt_transmit,
        .driver_free_rx_buffer = mgmt_free_rx,
    };
    esp_netif_inherent_config_t base = {
        .flags = 0,                         /* no DHCP client: the address is the device's */
        .if_key = "MGMT",
        .if_desc = "shared-ip",
        .route_prio = 100,
    };
    memcpy(base.mac, sta_mac, 6);
    const esp_netif_config_t cfg = {
        .base = &base,
        .driver = &driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_mgmt = esp_netif_new(&cfg);
    /* base.mac is not copied into the lwIP netif: without this every frame
     * the WT32 sends (and the sender address in its ARP) says 00:00:00:00:00:00,
     * which the access point drops. The Ethernet and Wi-Fi glue do the same. */
    uint8_t mac[6];
    memcpy(mac, sta_mac, 6);
    ESP_ERROR_CHECK(esp_netif_set_mac(s_mgmt, mac));
    esp_netif_action_start(s_mgmt, NULL, 0, NULL);
    esp_netif_action_connected(s_mgmt, NULL, 0, NULL);
    esp_netif_get_mac(s_mgmt, mac);        /* what lwIP actually sends with */
    ESP_LOGI(TAG, "management interface MAC " MACSTR, MAC2STR(mac));
}

/* Follow the device's address (its DHCP lease, else its static address, see
 * l2rewrite.h): the WT32 answers on the same IP. Until it is known to work on
 * the Wi-Fi network (none yet, too early to tell, or a static address from
 * another network) the page is only reachable through the setup AP, so keep
 * that up; only a definite "no" is reported. */
static void mgmt_ip_task(void *arg)
{
    bool mdns_on = false;
    bool announce = false;                  /* a new address waits for mDNS */
    l2rw_reach_t reported = L2RW_REACH_UNKNOWN;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        l2rw_addr_t a;
        uint32_t now = now_ms();
        portENTER_CRITICAL(&s_rw_lock);
        l2rw_mgmt_addr(&s_rw, now, &a);
        portEXIT_CRITICAL(&s_rw_lock);
        s_mgmt_reach = a.reach;
        s_mgmt_from_lease = a.from_lease;
        wifi_setup_request_ap(a.reach != L2RW_REACH_YES);
        esp_ip4_addr_t dev = { .addr = a.ip };
        if (a.reach == L2RW_REACH_NO && reported != L2RW_REACH_NO) {
            ESP_LOGW(TAG, "device address " IPSTR " is static and no other host of its subnet is on the "
                     "Wi-Fi network: keeping the setup AP up", IP2STR(&dev));
            reported = L2RW_REACH_NO;
        } else if (a.reach == L2RW_REACH_YES && reported == L2RW_REACH_NO) {
            ESP_LOGI(TAG, "device address " IPSTR " works on the Wi-Fi network", IP2STR(&dev));
            reported = L2RW_REACH_YES;
        } else if (a.reach == L2RW_REACH_YES) {
            reported = L2RW_REACH_YES;
        }
        esp_netif_ip_info_t want = {
            .ip.addr = a.ip,
            .netmask.addr = a.mask,
            .gw.addr = a.gw,
        };
        if (memcmp(&want, &s_mgmt_ip, sizeof(want)) != 0) {
            esp_netif_set_ip_info(s_mgmt, &want);
            if (a.dns) {
                esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = a.dns };
                esp_netif_set_dns_info(s_mgmt, ESP_NETIF_DNS_MAIN, &dns);
            }
            portENTER_CRITICAL(&s_demux_lock);
            demux_set_local_ip(&s_demux, a.ip);
            portEXIT_CRITICAL(&s_demux_lock);
            s_mgmt_ip = want;
            announce = a.ip != 0;
            if (a.ip) {
                ESP_LOGI(TAG, "management at http://" IPSTR ":%d (shared with the device)",
                         IP2STR(&want.ip), CONFIG_WT32_MGMT_PORT);
            } else {
                ESP_LOGI(TAG, "device address unknown, management on the setup AP only");
            }
        }
        /* mDNS only after wifi_setup_start() has initialised it: this task starts
         * first, and a device that sends its address right after link-up (a PC
         * does) made mdns_register_netif() race mdns_init() and assert on its
         * not yet created lock (seen on hardware, 3.7 s after boot). ENABLE
         * on a running interface probes and announces the new address; no
         * DISABLE: on a registered netif it clears the registration. */
        if (announce && wifi_setup_started()) {
            if (!mdns_on) {
                esp_err_t err = mdns_register_netif(s_mgmt);
                if (err == ESP_OK) {
                    mdns_on = true;
                } else {
                    ESP_LOGW(TAG, "mDNS on the management address: %s", esp_err_to_name(err));
                }
            }
            if (mdns_on) {
                mdns_netif_action(s_mgmt, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
            }
            announce = false;
        }
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    bool up = id == WIFI_EVENT_STA_CONNECTED;
    if (up) {
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_input);
        s_wifi_up = true;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
    } else {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_rw_lock);
    l2rw_wifi_state(&s_rw, up, now);            /* the reachability grace period counts from here */
    portEXIT_CRITICAL(&s_rw_lock);
}

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
        s_eth_up = true;
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Ethernet link down");
        s_eth_up = false;
        /* A different device may be plugged in next. */
        s_relearn = true;
    }
}

esp_err_t client_mode_start(esp_eth_handle_t eth)
{
    s_eth = eth;
    uint8_t sta_mac[6];
    esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
#if CONFIG_WT32_FORWARD_IPV6
    l2rw_init(&s_rw, sta_mac, true);
#else
    l2rw_init(&s_rw, sta_mac, false);
#endif
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL));
    ESP_LOGI(TAG, "station MAC " MACSTR " (the home router will see the device under it)", MAC2STR(sta_mac));
    demux_init(&s_demux, CONFIG_WT32_MGMT_PORT);
    mgmt_netif_init(sta_mac);
    xTaskCreate(mgmt_ip_task, "mgmt_ip", 3072, NULL, 3, NULL);
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth, eth_input, NULL));
    return eth_start_promiscuous(s_eth);
}

void client_mode_get_stats(client_stats_t *out)
{
    out->wifi_up = s_wifi_up;
    out->eth_up = s_eth_up;
    out->dev_known = s_rw.dev_known;
    memcpy(out->dev_mac, s_rw.dev_mac, 6);
    out->dev_ip = s_rw.dev_ip;
    out->to_wifi_frames = s_st.to_wifi_frames;
    out->to_wifi_bytes = s_st.to_wifi_bytes;
    out->to_eth_frames = s_st.to_eth_frames;
    out->to_eth_bytes = s_st.to_eth_bytes;
    out->drop_wifi_down = s_st.drop_wifi_down;
    out->drop_eth_down = s_st.drop_eth_down;
    out->tx_err_wifi = s_st.tx_err_wifi;
    out->tx_err_eth = s_st.tx_err_eth;
    out->tx_wait_wifi = s_st.wifi_tx_waits;
    out->tx_err_wifi_last = s_st.wifi_tx_last_err;
    out->foreign_frames = s_rw.foreign_frames;
    out->ipv6_dropped = s_rw.ipv6_dropped;
    out->dhcp_rewrites = s_rw.dhcp_rewrites;
    out->mgmt_ip = s_mgmt_ip.ip.addr;
    out->mgmt_reach = (int)s_mgmt_reach;
    out->dev_ip_leased = s_mgmt_from_lease;
    out->mgmt_rx_frames = s_st.mgmt_rx_frames;
    out->mgmt_tx_frames = s_st.mgmt_tx_frames;
    out->mgmt_tx_err = s_st.mgmt_tx_err;
    out->mgmt_tx_waits = s_st.mgmt_tx_waits;
    out->mgmt_tx_last_err = s_st.mgmt_tx_last_err;
    portENTER_CRITICAL(&s_demux_lock);
    out->mgmt_flows = demux_active_flows(&s_demux, now_ms());
    out->mgmt_evictions = s_demux.evictions;
    portEXIT_CRITICAL(&s_demux_lock);
}

void client_mode_relearn(void)
{
    /* Applied by the EMAC RX task before the next frame. */
    s_relearn = true;
}
