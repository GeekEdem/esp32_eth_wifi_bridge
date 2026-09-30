/* One button: debouncing and press lengths (pure C, host-testable).
 *
 *   release after < BTN_SHORT_MAX_MS          -> BTN_SHORT
 *   release after BTN_SETUP_MS..BTN_RESET_MS  -> BTN_SETUP
 *   release after >= BTN_RESET_MS             -> BTN_RESET
 *   anything in between                       -> nothing (a way to cancel)
 *
 * Acting on release lets the user see (on the display) what a hold will do
 * and let go in time. A button already held when sampling starts (stuck,
 * or pressed during power-on) is ignored until it has been seen released.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BTN_DEBOUNCE_MS   30
#define BTN_SHORT_MAX_MS  1000
#define BTN_SETUP_MS      5000
#define BTN_RESET_MS      10000

typedef enum {
    BTN_NONE,
    BTN_SHORT,
    BTN_SETUP,
    BTN_RESET,
} btn_event_t;

typedef struct {
    bool armed;                 /* seen released at least once */
    bool raw;                   /* last raw sample */
    uint32_t raw_since;         /* when the raw sample last changed */
    bool pressed;               /* debounced state */
    uint32_t pressed_at;
} btn_t;

void btn_init(btn_t *b, uint32_t now_ms);

/* Feed one sample (down = pressed); returns the event completed by it. */
btn_event_t btn_update(btn_t *b, bool down, uint32_t now_ms);

/* How long the current press lasts, 0 when not pressed (or not armed). */
uint32_t btn_held_ms(const btn_t *b, uint32_t now_ms);
