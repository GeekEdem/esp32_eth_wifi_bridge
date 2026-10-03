/* Control of the target's EN and IO0 lines.
 *
 * Both lines are driven open-drain: "asserted" pulls the line low, "released"
 * leaves it floating so the target's own pull-ups decide the level. On the
 * WT32-ETH01 this matters for IO0, which carries the 50 MHz Ethernet clock
 * once the application runs - it must never be driven high.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t target_ctl_init(void);

/* Low-level line control, as mapped from RFC2217 RTS (EN) and DTR (IO0).
 * EN asserted for longer than CONFIG_C3PROG_EN_HOLD_MAX_MS without another
 * call to target_set_en()/target_set_boot()/target_ctl_touch() is released
 * (with IO0): a client that vanished must not keep the target in reset. */
void target_set_en(bool asserted);
void target_set_boot(bool asserted);
bool target_en_asserted(void);
bool target_boot_asserted(void);
/* The client sent a control request: restart the EN watchdog. */
void target_ctl_touch(void);

/* Locally timed sequences. */
void target_enter_bootloader(void);
void target_reset(void);

/* Release both lines immediately (end of session). */
void target_release(void);
