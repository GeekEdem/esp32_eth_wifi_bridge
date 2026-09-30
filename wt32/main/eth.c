/* WT32-ETH01 Ethernet driver setup. */
#include "eth.h"

#include "driver/gpio.h"
#include "esp_eth_phy_lan87xx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

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
    return esp_eth_driver_install(&config, out);
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
