/* See button.h. */
#include "button.h"

void btn_init(btn_t *b, uint32_t now_ms)
{
    b->armed = false;
    b->raw = true;              /* assume held until a release is seen */
    b->raw_since = now_ms;
    b->pressed = true;
    b->pressed_at = now_ms;
}

btn_event_t btn_update(btn_t *b, bool down, uint32_t now_ms)
{
    if (down != b->raw) {
        b->raw = down;
        b->raw_since = now_ms;
    }
    if (b->raw == b->pressed || now_ms - b->raw_since < BTN_DEBOUNCE_MS) {
        return BTN_NONE;
    }
    b->pressed = b->raw;
    if (b->pressed) {
        b->pressed_at = b->raw_since;
        return BTN_NONE;
    }
    if (!b->armed) {
        b->armed = true;        /* the first release only arms the button */
        return BTN_NONE;
    }
    uint32_t held = b->raw_since - b->pressed_at;
    if (held < BTN_SHORT_MAX_MS) {
        return BTN_SHORT;
    }
    if (held >= BTN_RESET_MS) {
        return BTN_RESET;
    }
    return held >= BTN_SETUP_MS ? BTN_SETUP : BTN_NONE;
}

uint32_t btn_held_ms(const btn_t *b, uint32_t now_ms)
{
    return b->armed && b->pressed ? now_ms - b->pressed_at : 0;
}
