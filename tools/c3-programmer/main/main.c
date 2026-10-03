/* ESP32-C3 SuperMini as a programmer and serial probe for WT32-ETH01.
 *
 * - RFC2217 server: esptool/idf.py flash and monitor the target via
 *   rfc2217://<hostname>.local:<port>
 * - USB-Serial/JTAG <-> target UART at a fixed baud as a wired fallback
 * - Web page at http://<hostname>.local: status, target reset/bootloader;
 *   without a saved network a setup AP with a captive portal
 * - BOOT button: < 1 s target bootloader, 1-5 s target reset,
 *   5-10 s restart the programmer, >= 10 s forget Wi-Fi and reset the page
 *   password to 12345678
 * - LED: double blink = setup AP, 1 Hz = joining Wi-Fi, on = ready,
 *   flicker = serial traffic
 */
#include "bridge.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "target_ctl.h"
#include "setup_portal.h"
#include "web.h"
#include "wifi_setup.h"

#define BTN_BOOTLOADER_MAX_MS 1000
#define BTN_RESET_MAX_MS      5000
#define BTN_RESTART_MAX_MS    10000

static const char *TAG = "c3prog";

static void led_set(bool on)
{
#if CONFIG_C3PROG_LED_GPIO >= 0
#if CONFIG_C3PROG_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_C3PROG_LED_GPIO, !on);
#else
    gpio_set_level(CONFIG_C3PROG_LED_GPIO, on);
#endif
#endif
}

static void led_task(void *arg)
{
    uint32_t last = bridge_activity();
    unsigned tick = 0;   /* 50 ms */
    bool flicker = false;
    while (true) {
        uint32_t now = bridge_activity();
        if (now != last) {
            last = now;
            flicker = !flicker;
            led_set(flicker);
        } else if (wifi_setup_ap_active()) {
            unsigned phase = tick % 30;               /* double blink every 1.5 s */
            led_set(phase < 2 || (phase >= 4 && phase < 6));
        } else if (wifi_setup_state() != WIFI_SETUP_CONNECTED) {
            led_set((tick / 10) % 2);                 /* 1 Hz */
        } else {
            led_set(true);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void button_task(void *arg)
{
    const int gpio = CONFIG_C3PROG_BUTTON_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    const TickType_t step = pdMS_TO_TICKS(20);
    /* Down already at start is not a press: the >= 10 s hold below restarts
     * the C3 with the button still held, and the rest of that hold counted
     * as a new one restarted the WT32 on release (seen on hardware). */
    do {
        vTaskDelay(step);
    } while (gpio_get_level(gpio) == 0);
    while (true) {
        vTaskDelay(step);
        if (gpio_get_level(gpio) != 0) {
            continue;
        }
        unsigned held_ms = 0;
        while (gpio_get_level(gpio) == 0) {
            vTaskDelay(step);
            held_ms += 20;
            if (held_ms >= BTN_RESTART_MAX_MS) {
                ESP_LOGW(TAG, "button held 10 s, forgetting Wi-Fi and the page password");
                wifi_setup_forget();
                setup_portal_reset_password();
                target_release();
                esp_restart();
            }
        }
        if (held_ms < 40) {
            continue;   /* bounce */
        } else if (held_ms < BTN_BOOTLOADER_MAX_MS) {
            target_enter_bootloader();
        } else if (held_ms < BTN_RESET_MAX_MS) {
            target_reset();
        } else {
            ESP_LOGW(TAG, "button held 5 s, restarting programmer");
            target_release();
            esp_restart();
        }
    }
}

void app_main(void)
{
    /* First thing: take control of EN/IO0 in the released state. */
    ESP_ERROR_CHECK(target_ctl_init());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_C3PROG_LED_GPIO >= 0
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << CONFIG_C3PROG_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
#endif
#if CONFIG_C3PROG_BUTTON_GPIO >= 0
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
#endif

    ESP_ERROR_CHECK(bridge_start());
    const wifi_setup_config_t wifi = {
        .ap_prefix = "C3prog-Setup",
        .default_ssid = CONFIG_C3PROG_WIFI_SSID,
        .default_password = CONFIG_C3PROG_WIFI_PASSWORD,
        .default_hostname = CONFIG_C3PROG_HOSTNAME,
        .mdns_instance = "WT32 programmer",
        .sta_netif = true,
        .tx_power = CONFIG_C3PROG_WIFI_TX_POWER,
    };
    ESP_ERROR_CHECK(wifi_setup_start(&wifi));
    ESP_ERROR_CHECK(web_start());
}
