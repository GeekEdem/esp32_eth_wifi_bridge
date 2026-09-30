/* Optional SSD1306 128x64 I2C display and one button.
 *
 * The display is found at 0x3C or 0x3D; without it the button still works.
 * Short press: wake the screen / next page. Hold 5 s: one-time setup start;
 * hold 10 s: reset of the settings (see button.h: acting on release). The
 * screen goes dark after timeout_s without a press (OLED burn-in). */
#pragma once

#include <stdint.h>
#include "disp_ui.h"
#include "esp_err.h"

typedef struct {
    int sda_gpio, scl_gpio;         /* -1: no display */
    int button_gpio;                /* to GND, internal pull-up; -1: none */
    uint32_t timeout_s;             /* 0: never dark */
    void (*fill)(ui_info_t *in);    /* current data for the pages */
    void (*on_setup)(void);         /* hold 5 s */
    void (*on_reset)(void);         /* hold 10 s */
    const char *texts;              /* language: its JSON texts (see ui_set_texts()) */
    size_t texts_len;
} display_config_t;

esp_err_t display_start(const display_config_t *cfg);

/* Another language (the JSON must stay valid, e.g. the built-in bundle);
 * taken by the display task before it draws next. */
void display_set_texts(const char *json, size_t len);
