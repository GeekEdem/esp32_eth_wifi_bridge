/* Target EN/IO0 control with a local IO0 hold after reset release. */
#include "target_ctl.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define EN_GPIO        CONFIG_C3PROG_EN_GPIO
#define BOOT_GPIO      CONFIG_C3PROG_BOOT_GPIO
#define BOOT_HOLD_US   ((int64_t)CONFIG_C3PROG_BOOT_HOLD_MS * 1000)
#define RESET_PULSE_MS 100

static const char *TAG = "target";

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_boot_release_timer;
static bool s_en_asserted;
static bool s_boot_asserted;
static bool s_boot_release_pending;
static int64_t s_en_release_us;   /* when EN was released while IO0 was low, else 0 */

/* Level 1 = released (open-drain high-Z), 0 = pulled low. */
static inline void drive(int gpio, bool asserted)
{
    gpio_set_level(gpio, asserted ? 0 : 1);
}

static void boot_release_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_boot_release_pending) {
        s_boot_release_pending = false;
        s_boot_asserted = false;
        s_en_release_us = 0;
        drive(BOOT_GPIO, false);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t target_ctl_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mutex");

    /* Latch "released" into the output register before enabling the driver,
     * so the target does not see a reset pulse when the C3 boots. */
    gpio_set_level(EN_GPIO, 1);
    gpio_set_level(BOOT_GPIO, 1);
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << EN_GPIO) | (1ULL << BOOT_GPIO),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    drive(EN_GPIO, false);
    drive(BOOT_GPIO, false);

    const esp_timer_create_args_t targs = {
        .callback = boot_release_cb,
        .name = "boot_hold",
    };
    return esp_timer_create(&targs, &s_boot_release_timer);
}

void target_set_en(bool asserted)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (asserted) {
        s_en_release_us = 0;
    } else if (s_en_asserted && s_boot_asserted) {
        s_en_release_us = esp_timer_get_time();
    }
    s_en_asserted = asserted;
    drive(EN_GPIO, asserted);
    xSemaphoreGive(s_lock);
}

void target_set_boot(bool asserted)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (asserted) {
        s_boot_release_pending = false;
        esp_timer_stop(s_boot_release_timer);
        s_boot_asserted = true;
        drive(BOOT_GPIO, true);
    } else if (s_boot_asserted && !s_boot_release_pending) {
        int64_t since = s_en_release_us ? esp_timer_get_time() - s_en_release_us : BOOT_HOLD_US;
        if (since < BOOT_HOLD_US) {
            /* The target has just left reset with IO0 low: keep it low until
             * the strapping pins are safely latched, then let go. */
            s_boot_release_pending = true;
            esp_timer_start_once(s_boot_release_timer, BOOT_HOLD_US - since);
        } else {
            s_boot_asserted = false;
            s_en_release_us = 0;
            drive(BOOT_GPIO, false);
        }
    }
    xSemaphoreGive(s_lock);
}

bool target_en_asserted(void)
{
    return s_en_asserted;
}

bool target_boot_asserted(void)
{
    return s_boot_asserted;
}

void target_enter_bootloader(void)
{
    ESP_LOGI(TAG, "entering target bootloader");
    target_set_boot(true);
    target_set_en(true);
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    target_set_en(false);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_C3PROG_BOOT_HOLD_MS + 10));
    target_set_boot(false);
}

void target_reset(void)
{
    ESP_LOGI(TAG, "resetting target");
    target_release();
    target_set_en(true);
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    target_set_en(false);
}

void target_release(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_boot_release_pending = false;
    esp_timer_stop(s_boot_release_timer);
    s_en_asserted = false;
    s_boot_asserted = false;
    s_en_release_us = 0;
    drive(EN_GPIO, false);
    drive(BOOT_GPIO, false);
    xSemaphoreGive(s_lock);
}
