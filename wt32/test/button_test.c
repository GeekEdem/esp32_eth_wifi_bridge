/* Host test of button.c: debouncing, press lengths, stuck-at-boot guard. */
#include <assert.h>
#include <stdio.h>
#include "button.h"

static uint32_t t;
static btn_t b;

/* Hold the level for ms, sampling every 10 ms; returns the last non-NONE event. */
static btn_event_t hold(bool down, uint32_t ms)
{
    btn_event_t ev = BTN_NONE;
    for (uint32_t n = 0; n < ms; n += 10, t += 10) {
        btn_event_t e = btn_update(&b, down, t);
        if (e != BTN_NONE) {
            assert(ev == BTN_NONE);         /* at most one event per hold */
            ev = e;
        }
    }
    return ev;
}

static btn_event_t press(uint32_t ms)
{
    assert(hold(true, ms) == BTN_NONE);     /* events come on release only */
    return hold(false, 100);
}

int main(void)
{
    t = 1000;
    btn_init(&b, t);
    assert(hold(false, 100) == BTN_NONE);   /* released at start: arms, no event */

    assert(press(50) == BTN_SHORT);
    assert(press(900) == BTN_SHORT);
    assert(press(1500) == BTN_NONE);        /* between short and setup: cancelled */
    assert(press(4900) == BTN_NONE);
    assert(press(5100) == BTN_SETUP);
    assert(press(9900) == BTN_SETUP);
    assert(press(10100) == BTN_RESET);
    assert(press(30000) == BTN_RESET);

    /* bounces shorter than the debounce time are ignored */
    for (int i = 0; i < 5; i++) {
        btn_update(&b, true, t); t += 10;
        btn_update(&b, false, t); t += 10;
    }
    assert(hold(false, 100) == BTN_NONE);
    /* a bouncy press still counts once */
    btn_update(&b, true, t); t += 5; btn_update(&b, false, t); t += 5;
    assert(hold(true, 200) == BTN_NONE);
    btn_update(&b, false, t); t += 5; btn_update(&b, true, t); t += 5;
    assert(hold(false, 100) == BTN_SHORT);

    /* held time is visible while pressed */
    hold(true, 2000);
    assert(btn_held_ms(&b, t) >= 1990 && btn_held_ms(&b, t) <= 2010);
    assert(hold(false, 100) == BTN_NONE);
    assert(btn_held_ms(&b, t) == 0);

    /* held at power-on (e.g. for 20 s): no reset when let go */
    t = 5000;
    btn_init(&b, t);
    assert(btn_held_ms(&b, t) == 0);
    assert(hold(true, 20000) == BTN_NONE);
    assert(btn_held_ms(&b, t) == 0);
    assert(hold(false, 100) == BTN_NONE);
    assert(press(200) == BTN_SHORT);        /* works normally afterwards */

    /* clock wrap-around */
    t = 0xFFFFFF00u;
    btn_init(&b, t);
    hold(false, 100);
    assert(press(6000) == BTN_SETUP);

    puts("all button tests passed");
    return 0;
}
