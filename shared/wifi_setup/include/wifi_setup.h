/* Wi-Fi station with credentials in NVS, plus a setup access point.
 *
 * - No saved network: setup AP "<ap_prefix>-XXXX" with a captive portal.
 * - Saved network unreachable for 60 s: the setup AP comes up next to the
 *   station, which keeps retrying; it goes away once connected (unless
 *   ap_always_on).
 *
 * With sta_netif = false the station carries no IP stack: the application
 * owns the station's frames (L2 bridge) and "connected" means associated.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    const char *ap_prefix;          /* "C3prog-Setup" -> "C3prog-Setup-1A2B" */
    const char *default_ssid;       /* used while nothing is saved; may be "" */
    const char *default_password;
    const char *default_hostname;
    const char *mdns_instance;
    bool sta_netif;
    bool ap_always_on;
    int tx_power;                   /* 0.25 dBm units, 0 = driver default */
} wifi_setup_config_t;

typedef enum {
    WIFI_SETUP_UNCONFIGURED,        /* no saved network, setup AP only */
    WIFI_SETUP_CONNECTING,          /* joining the saved network */
    WIFI_SETUP_CONNECTED,
} wifi_setup_state_t;

esp_err_t wifi_setup_start(const wifi_setup_config_t *cfg);

/* Keep the setup AP up for an application reason (e.g. its page is not
 * reachable in the home network yet); false hands control back. */
void wifi_setup_request_ap(bool on);

wifi_setup_state_t wifi_setup_state(void);
bool wifi_setup_ap_active(void);
const char *wifi_setup_ssid(void);
const char *wifi_setup_hostname(void);
const char *wifi_setup_ap_ssid(void);
/* Station IPv4 as text, "" when not connected or without a station netif. */
void wifi_setup_ip(char *buf, size_t len);
int wifi_setup_rssi(void);

/* Blocking scan; returns the number of records written. */
int wifi_setup_scan(wifi_ap_record_t *out, int max);

/* Persist settings; applied after restart. */
esp_err_t wifi_setup_save(const char *ssid, const char *password, const char *hostname);
esp_err_t wifi_setup_forget(void);
