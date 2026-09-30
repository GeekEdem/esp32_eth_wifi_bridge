/* WT32-ETH01: Wi-Fi for one Ethernet device (or, as an access point, for the
 * network on the cable), in one of three modes (chosen on the web page,
 * stored in NVS, applied after restart):
 *
 * - Client (default): joins an existing Wi-Fi; the device gets its IP from
 *   that network's router and is reachable like a wired client. The WT32
 *   shares the device's IP and answers only on the management port
 *   (http://<device IP>:28480). Setup AP "WT32-Setup-XXXX" (http://192.168.4.1)
 *   while no network is saved, the network is unreachable, or the device's
 *   address is not known yet.
 * - Router ("own"): the WT32 is an access point; the AP and Ethernet form one
 *   network with the WT32's DHCP server (WT32 at <own_ip>, page on port 80).
 * - Access point ("ap"): the cable goes to a router; the WT32's AP extends
 *   that network (DHCP, internet: all from the router). Page at the address
 *   the WT32 gets there; <own_ip> as a fallback when there is no DHCP.
 *
 * Optional SSD1306 display and a button (display.c): pages with the state;
 * holding the button 5 s restarts once into the client mode with the setup
 * AP up (a way in when the page is out of reach), 10 s erases the settings.
 *
 * A summary is logged to UART every 10 s.
 */
#include <string.h>

#include "client_mode.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eth.h"
#include "setup_portal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "own_mode.h"
#include "script.h"
#include "sdkconfig.h"
#include "settings.h"
#include "web.h"
#include "wifi_setup.h"

static const char *TAG = "wt32";

/* Set by the button before a restart: the next start is a setup start. */
#define SETUP_BOOT_MAGIC 0x5E7B0071u
static RTC_NOINIT_ATTR uint32_t s_setup_boot_flag;
static bool s_setup_boot;
static wt32_mode_t s_mode;

static const char SCRIPT_EXAMPLE[] =
    "# WT32 script example (Berry language: https://berry.readthedocs.io).\n"
    "# output(key, value) shows a result on the page; print() writes to the console.\n"
    "# status() returns the device state (mode, eth, devIp, devMac, ...).\n"
    "\n"
    "def show()\n"
    "  var s = status()\n"
    "  var names = {\"client\": \"client\", \"own\": \"router\", \"ap\": \"access point\"}\n"
    "  output(\"Mode\", names.find(s[\"mode\"], s[\"mode\"]))\n"
    "  output(\"Ethernet\", s[\"eth\"] ? \"link up\" : \"no link\")\n"
    "  output(\"Device IP\", s[\"devIp\"] != \"\" ? s[\"devIp\"] : \"-\")\n"
    "  output(\"Free memory\", str(heap() / 1024) + \" KB\")\n"
    "end\n"
    "\n"
    "show()\n"
    "every(5000, show)\n"
    "print(\"script started\")\n";

static void report_client(void)
{
    client_stats_t s;
    client_mode_get_stats(&s);
    ip4_addr_t ip = { .addr = s.dev_ip };
    ip4_addr_t mgmt = { .addr = s.mgmt_ip };
    ESP_LOGI(TAG, "client | wifi %s rssi %d | eth %s | device " MACSTR " " IPSTR " (%s)"
             " | ->wifi %lu fr / %lu B, ->eth %lu fr / %lu B | drops wifi %lu eth %lu, "
             "tx err %lu/%lu (last %s), waited for wifi %lu, foreign %lu, ipv6 %lu, dhcp rw %lu | mgmt " IPSTR ":%d "
             "rx %lu tx %lu err %lu flows %d%s",
             s.wifi_up ? "up" : "down", wifi_setup_rssi(), s.eth_up ? "up" : "down",
             MAC2STR(s.dev_mac), IP2STR(&ip), s.dev_ip_leased ? "DHCP" : "static",
             (unsigned long)s.to_wifi_frames, (unsigned long)s.to_wifi_bytes,
             (unsigned long)s.to_eth_frames, (unsigned long)s.to_eth_bytes,
             (unsigned long)s.drop_wifi_down, (unsigned long)s.drop_eth_down,
             (unsigned long)s.tx_err_wifi, (unsigned long)s.tx_err_eth,
             s.tx_err_wifi_last ? esp_err_to_name(s.tx_err_wifi_last) : "-", (unsigned long)s.tx_wait_wifi,
             (unsigned long)s.foreign_frames, (unsigned long)s.ipv6_dropped,
             (unsigned long)s.dhcp_rewrites, IP2STR(&mgmt), CONFIG_WT32_MGMT_PORT,
             (unsigned long)s.mgmt_rx_frames, (unsigned long)s.mgmt_tx_frames,
             (unsigned long)s.mgmt_tx_err, s.mgmt_flows,
             s.mgmt_ip && !s.mgmt_reachable ? " | not on the Wi-Fi network: setup AP up" : "");
}

static void report_own(void)
{
    own_stats_t s;
    own_mode_get_stats(&s);
    ip4_addr_t ip = { .addr = s.dev_ip };
    ESP_LOGI(TAG, "router \"%s\" | eth %s | wifi clients %d | device " MACSTR " " IPSTR,
             s.ssid, s.eth_up ? "up" : "down", s.ap_clients, MAC2STR(s.dev_mac), IP2STR(&ip));
}

static void report_ap(void)
{
    static const char *const UPLINK[] = { "-", "waiting for DHCP", "DHCP", "fallback" };
    own_stats_t s;
    own_mode_get_stats(&s);
    ip4_addr_t ip = { .addr = s.ip }, gw = { .addr = s.gw };
    ESP_LOGI(TAG, "access point \"%s\" | eth %s | wifi clients %d | WT32 " IPSTR " (%s) gw " IPSTR,
             s.ssid, s.eth_up ? "up" : "down", s.ap_clients, IP2STR(&ip), UPLINK[s.uplink], IP2STR(&gw));
}

/* ---------- display and button ---------- */

static void fill_info(ui_info_t *in)
{
    in->setup_boot = s_setup_boot;
    strlcpy(in->version, esp_app_get_description()->version, sizeof(in->version));
    in->heap = esp_get_free_heap_size();
    in->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    while (in->n_out < UI_MAX_OUT &&
           script_output_get(in->n_out, in->out_key[in->n_out], sizeof(in->out_key[0]),
                             in->out_val[in->n_out], sizeof(in->out_val[0]))) {
        in->n_out++;
    }
    wt32_eth_link_t link;
    eth_link_get(&link);
    in->eth_speed = link.up ? link.speed_mbps : 0;
    in->eth_full = link.full_duplex;
    if (s_mode == WT32_MODE_CLIENT) {
        client_stats_t s;
        client_mode_get_stats(&s);
        wifi_setup_state_t st = wifi_setup_state();
        in->mode = UI_CLIENT;
        in->wifi_state = st == WIFI_SETUP_CONNECTED ? 2 : st == WIFI_SETUP_CONNECTING ? 1 : 0;
        strlcpy(in->wifi_ssid, wifi_setup_ssid(), sizeof(in->wifi_ssid));
        in->rssi = wifi_setup_rssi();
        in->setup_ap = wifi_setup_ap_active();
        strlcpy(in->setup_ssid, wifi_setup_ap_ssid(), sizeof(in->setup_ssid));
        in->mgmt_ip = s.mgmt_reachable ? s.mgmt_ip : 0;    /* otherwise the display points to the setup AP */
        in->mgmt_port = CONFIG_WT32_MGMT_PORT;
        in->to_wifi_bytes = s.to_wifi_bytes;
        in->to_eth_bytes = s.to_eth_bytes;
        in->dropped = s.drop_wifi_down + s.drop_eth_down;
        in->tx_errors = s.tx_err_wifi + s.tx_err_eth;
        in->foreign = s.foreign_frames;
        in->eth_up = s.eth_up;
        in->dev_known = s.dev_known;
        memcpy(in->dev_mac, s.dev_mac, 6);
        in->dev_ip = s.dev_ip;
        return;
    }
    own_stats_t s;
    own_mode_get_stats(&s);
    in->mode = s_mode == WT32_MODE_AP ? UI_AP : UI_ROUTER;
    strlcpy(in->ap_ssid, s.ssid, sizeof(in->ap_ssid));
    in->ap_clients = s.ap_clients;
    in->wt32_ip = s.ip;
    in->gw = s.gw;
    in->uplink = (ui_uplink_t)s.uplink;     /* same order as uplink_t */
    in->eth_up = s.eth_up;
    in->dev_known = s.dev_known;
    memcpy(in->dev_mac, s.dev_mac, 6);
    in->dev_ip = s.dev_ip;
}

static void on_setup(void)
{
    s_setup_boot_flag = SETUP_BOOT_MAGIC;
    esp_restart();
}

static void on_reset(void)
{
    /* Everything in NVS: mode, networks, page password, DHCP leases, script
     * autostart. The script text itself (storage partition) stays. */
    esp_err_t err = nvs_flash_erase();
    ESP_LOGW(TAG, "settings erased (%s), restarting", esp_err_to_name(err));
    esp_restart();
}

static void report_task(void *arg)
{
    wt32_mode_t mode = (wt32_mode_t)(intptr_t)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (mode == WT32_MODE_OWN) {
            report_own();
        } else if (mode == WT32_MODE_AP) {
            report_ap();
        } else {
            report_client();
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_setup_boot = s_setup_boot_flag == SETUP_BOOT_MAGIC && esp_reset_reason() == ESP_RST_SW;
    s_setup_boot_flag = 0;

    wt32_settings_t set;
    settings_load(&set);
    if (s_setup_boot) {
        ESP_LOGW(TAG, "one-time setup start (button): client mode with the setup AP");
        set.mode = WT32_MODE_CLIENT;            /* this boot only; NVS keeps the saved mode */
    }
    if (set.mode != WT32_MODE_CLIENT && !settings_own_valid(&set)) {
        ESP_LOGW(TAG, "access point settings incomplete, starting in client mode");
        set.mode = WT32_MODE_CLIENT;
    }

    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(eth_init(&eth));

    if (set.mode != WT32_MODE_CLIENT) {
        ESP_ERROR_CHECK(own_mode_start(eth, &set));
    } else {
        ESP_ERROR_CHECK(client_mode_start(eth));
        const wifi_setup_config_t wifi = {
            .ap_prefix = "WT32-Setup",
            .default_ssid = CONFIG_WT32_WIFI_SSID,
            .default_password = CONFIG_WT32_WIFI_PASSWORD,
            .default_hostname = CONFIG_WT32_HOSTNAME,
            .mdns_instance = "WT32",
            .sta_netif = false,
            .ap_always_on = s_setup_boot,
        };
        ESP_ERROR_CHECK(wifi_setup_start(&wifi));
    }
    ESP_ERROR_CHECK(web_start(&set, s_setup_boot));
    s_mode = set.mode;

    const script_config_t script = {
        .mem_limit = CONFIG_WT32_SCRIPT_MEM_KB * 1024,
        .time_limit_ms = 2000,
        .status_json = web_status_extra,
        .example = SCRIPT_EXAMPLE,
    };
    ESP_ERROR_CHECK(script_start(&script));
    ESP_ERROR_CHECK(script_web_start());

    const portal_lang_t *lang = setup_portal_lang();       /* the language chosen on the page */
    const display_config_t disp = {
        .texts = lang ? lang->json : NULL,
        .texts_len = lang ? lang->json_len : 0,
        .sda_gpio = CONFIG_WT32_I2C_SDA_GPIO,
        .scl_gpio = CONFIG_WT32_I2C_SCL_GPIO,
        .button_gpio = CONFIG_WT32_BUTTON_GPIO,
        .timeout_s = CONFIG_WT32_DISPLAY_TIMEOUT_S,
        .fill = fill_info,
        .on_setup = on_setup,
        .on_reset = on_reset,
    };
    ESP_ERROR_CHECK(display_start(&disp));

    xTaskCreate(report_task, "report", 3072, (void *)(intptr_t)set.mode, 2, NULL);
}
