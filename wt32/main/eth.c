/* WT32-ETH01 Ethernet driver setup. */
#include "eth.h"

#include "driver/gpio.h"
#include "esp_eth_phy_lan87xx.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

static wt32_eth_link_t s_link;

#define PHY_ANLPAR          5           /* auto-negotiation link partner ability */
#define ANLPAR_PAUSE        (1u << 10)

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_link.up = false;
        return;
    }
    if (id != ETHERNET_EVENT_CONNECTED) {
        return;
    }
    esp_eth_handle_t eth = *(esp_eth_handle_t *)data;
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    uint32_t anlpar = 0;
    esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
    esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
    esp_eth_phy_reg_rw_data_t reg = { .reg_addr = PHY_ANLPAR, .reg_value_p = &anlpar };
    esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &reg);
    s_link = (wt32_eth_link_t){
        .up = true,
        .speed_mbps = speed == ETH_SPEED_100M ? 100 : 10,
        .full_duplex = duplex == ETH_DUPLEX_FULL,
        .pause = duplex == ETH_DUPLEX_FULL && (anlpar & ANLPAR_PAUSE),
    };
    /* The LAN8720's LED2 lights at 100 Mbit/s only; the PHY drives both LEDs
     * itself, the firmware has no say in them. */
    ESP_LOGI(TAG, "link up: %d Mbit/s, %s duplex, flow control %s (partner ability 0x%04lx)",
             s_link.speed_mbps, s_link.full_duplex ? "full" : "half",
             s_link.pause ? "on" : "off (the device does not take PAUSE frames)", (unsigned long)anlpar);
    if (!s_link.full_duplex || s_link.speed_mbps != 100) {
        ESP_LOGW(TAG, "not 100 Mbit/s full duplex: check the cable, or the device only supports this");
    }
}

void eth_link_get(wt32_eth_link_t *out)
{
    *out = s_link;
}

esp_err_t eth_init(esp_eth_handle_t *out)
{
#if CONFIG_WT32_ETH_PHY_POWER_GPIO >= 0
    /* Enables the 50 MHz oscillator feeding GPIO0 */
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << CONFIG_WT32_ETH_PHY_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(CONFIG_WT32_ETH_PHY_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_WT32_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_WT32_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_WT32_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, out);
    if (err != ESP_OK) {
        return err;
    }
    /* Flow control: when the frames from the device come faster than Wi-Fi
     * takes them, the receive side waits (client_mode.c), the EMAC's RX
     * descriptors fill up and it sends PAUSE frames, so the device slows down
     * instead of losing frames. The PHY advertises it before the link comes up;
     * it is active only if the device advertises it too (full duplex).
     * (esp_eth reads this argument both as bool and as uint32_t.) */
    uint32_t flow_ctrl = 1;
    err = esp_eth_ioctl(*out, ETH_CMD_S_FLOW_CTRL, &flow_ctrl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flow control: %s", esp_err_to_name(err));
    }
    return esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL);
}

esp_err_t eth_start_promiscuous(esp_eth_handle_t eth)
{
    esp_err_t err = esp_eth_start(eth);
    if (err != ESP_OK) {
        return err;
    }
    bool promiscuous = true;
    return esp_eth_ioctl(eth, ETH_CMD_S_PROMISCUOUS, &promiscuous);
}
