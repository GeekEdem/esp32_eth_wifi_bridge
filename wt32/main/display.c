/* See display.h. One task shows the start screen until display_start(), then
 * samples the button every 20 ms and redraws the screen twice a second while
 * it is on. */
#include "display.h"

#include <string.h>

#include "button.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TICK_MS     20
#define REDRAW_MS   500
#define BOOT_MS     60                      /* a frame of the start screen */
#define I2C_HZ      400000

static const char *TAG = "display";

static display_config_t s_cfg;
static i2c_master_dev_handle_t s_dev;       /* NULL: no display */
static uint8_t s_fb[UI_FB_SIZE];
static ui_info_t s_info;
static portMUX_TYPE s_texts_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *s_texts_new;             /* a language waiting for the display task */
static size_t s_texts_new_len;
static bool s_booted;                       /* display_boot() done: probed, task running */
static volatile bool s_running;             /* display_start() done: pages and the button */
static volatile int s_boot_done;
static const char *volatile s_boot_key;

static esp_err_t cmds(const uint8_t *c, size_t n)
{
    uint8_t buf[32] = { 0x00 };             /* control byte: commands */
    memcpy(buf + 1, c, n);
    return i2c_master_transmit(s_dev, buf, n + 1, 100);
}

static esp_err_t panel_init(void)
{
    static const uint8_t init[] = {
        0xAE,               /* off */
        0xD5, 0x80,         /* clock */
        0xA8, 0x3F,         /* 64 rows */
        0xD3, 0x00,         /* no offset */
        0x40,               /* start line 0 */
        0x8D, 0x14,         /* charge pump on */
        0x20, 0x00,         /* horizontal addressing */
        0xA1, 0xC8,         /* column / row remap: normal orientation */
        0xDA, 0x12,         /* COM pins for 128x64 */
        0x81, 0xCF,         /* contrast */
        0xD9, 0xF1,         /* pre-charge */
        0xDB, 0x40,         /* VCOMH */
        0xA4, 0xA6,         /* show RAM, not inverted */
        0xAF,               /* on */
    };
    return cmds(init, sizeof(init));
}

static void panel_power(bool on)
{
    if (s_dev) {
        uint8_t c = on ? 0xAF : 0xAE;
        cmds(&c, 1);
    }
}

static void flush(void)
{
    if (!s_dev) {
        return;
    }
    static const uint8_t window[] = { 0x21, 0, 127, 0x22, 0, 7 };
    static uint8_t buf[1 + UI_FB_SIZE];
    if (cmds(window, sizeof(window)) != ESP_OK) {
        return;
    }
    buf[0] = 0x40;                          /* control byte: data */
    memcpy(buf + 1, s_fb, UI_FB_SIZE);
    i2c_master_transmit(s_dev, buf, sizeof(buf), 200);
}

static void display_probe(void)
{
    if (s_cfg.sda_gpio < 0 || s_cfg.scl_gpio < 0) {
        return;
    }
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = s_cfg.sda_gpio,
        .scl_io_num = s_cfg.scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* modules usually have their own */
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus failed");
        return;
    }
    static const uint16_t addrs[] = { 0x3C, 0x3D };
    for (size_t i = 0; i < 2; i++) {
        if (i2c_master_probe(bus, addrs[i], 50) != ESP_OK) {
            continue;
        }
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) == ESP_OK && panel_init() == ESP_OK) {
            ESP_LOGI(TAG, "SSD1306 at 0x%02X (SDA %d, SCL %d)", addrs[i], s_cfg.sda_gpio, s_cfg.scl_gpio);
            return;
        }
        s_dev = NULL;
    }
    ESP_LOGI(TAG, "no display at 0x3C/0x3D (SDA %d, SCL %d)", s_cfg.sda_gpio, s_cfg.scl_gpio);
    i2c_del_master_bus(bus);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void notice(const char *title, const char *l1, const char *l2)
{
    ui_render_notice(s_fb, title, l1, l2, NULL);
    panel_power(true);
    flush();
}

void display_set_texts(const char *json, size_t len)
{
    portENTER_CRITICAL(&s_texts_lock);
    s_texts_new = json;
    s_texts_new_len = len;
    portEXIT_CRITICAL(&s_texts_lock);
}

/* In the display task only: ui_* keep the texts in static storage. */
static void take_texts(void)
{
    portENTER_CRITICAL(&s_texts_lock);
    const char *json = s_texts_new;
    size_t len = s_texts_new_len;
    s_texts_new = NULL;
    portEXIT_CRITICAL(&s_texts_lock);
    if (json && ui_set_texts(json, len) < 0) {
        ESP_LOGW(TAG, "language texts unreadable, keeping the previous ones");
    }
}

/* Until display_start(): what is starting, and a block running through the
 * bar so that a long step (the Ethernet start waits up to 4 s for a link)
 * still shows the WT32 is alive. */
static void boot_screen(void)
{
    const char *version = esp_app_get_description()->version;
    for (uint32_t frame = 0; !s_running; frame++) {
        if (s_texts_new) {
            take_texts();
        }
        const char *key = s_boot_key;
        ui_render_boot(s_fb, version, key ? ui_tr(key) : NULL, s_boot_done, DISPLAY_BOOT_STEPS, frame);
        flush();
        vTaskDelay(pdMS_TO_TICKS(BOOT_MS));
    }
}

static void ui_task(void *arg)
{
    if (s_dev) {
        boot_screen();
    }
    btn_t b;
    uint32_t t = now_ms();
    btn_init(&b, t);
    bool on = true;
    uint32_t last_press = t, next_draw = 0;
    int page = 0;
    bool holding = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        if (s_texts_new) {
            take_texts();
            next_draw = now_ms();                   /* redraw in the new language */
        }
        t = now_ms();
        bool down = s_cfg.button_gpio >= 0 && gpio_get_level(s_cfg.button_gpio) == 0;
        btn_event_t ev = btn_update(&b, down, t);
        uint32_t held = btn_held_ms(&b, t);

        if (held >= BTN_SHORT_MAX_MS) {             /* a hold: show what letting go does */
            if (!on) {
                panel_power(on = true);
            }
            last_press = t;
            holding = true;
            if ((int32_t)(t - next_draw) >= 0) {
                ui_render_hold(s_fb, held);
                flush();
                next_draw = t + 100;
            }
            continue;
        }
        if (ev != BTN_NONE || holding) {
            last_press = t;
            next_draw = t;                          /* redraw now */
        }
        holding = false;
        switch (ev) {
        case BTN_SHORT:
            if (on) {
                page++;
            } else {
                panel_power(on = true);
            }
            break;
        case BTN_SETUP:
            ESP_LOGW(TAG, "button held 5 s: one-time setup start");
            notice(ui_tr("disp.tSetup"), ui_tr("disp.restarting1"), ui_tr("disp.restarting2"));
            s_cfg.on_setup();
            break;
        case BTN_RESET:
            ESP_LOGW(TAG, "button held 10 s: reset of the settings");
            notice(ui_tr("disp.tReset"), ui_tr("disp.erasing"), ui_tr("disp.restarting"));
            s_cfg.on_reset();
            break;
        default:
            break;
        }
        if (on && s_cfg.timeout_s && t - last_press > s_cfg.timeout_s * 1000) {
            panel_power(on = false);
        }
        if (on && s_dev && (int32_t)(t - next_draw) >= 0) {
            memset(&s_info, 0, sizeof(s_info));
            s_cfg.fill(&s_info);
            ui_render_page(s_fb, &s_info, page);
            flush();
            next_draw = t + REDRAW_MS;
        }
    }
}

static void setup(const display_config_t *cfg)
{
    s_cfg = *cfg;
    if (s_cfg.texts) {
        display_set_texts(s_cfg.texts, s_cfg.texts_len);    /* the task takes it first */
    }
    display_probe();
}

esp_err_t display_boot(const display_config_t *cfg)
{
    setup(cfg);
    s_booted = true;
    if (!s_dev) {
        return ESP_OK;                              /* the button's task starts with display_start() */
    }
    return xTaskCreate(ui_task, "ui", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void display_boot_step(int done, const char *key)
{
    s_boot_key = key;
    s_boot_done = done;
}

esp_err_t display_start(const display_config_t *cfg)
{
    bool task = s_booted && s_dev;                  /* already running, on the start screen */
    if (!s_booted) {
        setup(cfg);
    }
    if (s_cfg.button_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_cfg.button_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "button GPIO");
    }
    s_running = true;                               /* the start screen ends, the pages begin */
    if (task || (!s_dev && s_cfg.button_gpio < 0)) {
        return ESP_OK;
    }
    return xTaskCreate(ui_task, "ui", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
