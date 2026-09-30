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

/* Low-level line control, as mapped from RFC2217 RTS (EN) and DTR (IO0). */
void target_set_en(bool asserted);
void target_set_boot(bool asserted);
bool target_en_asserted(void);
bool target_boot_asserted(void);

/* Locally timed sequences. */
void target_enter_bootloader(void);
void target_reset(void);

/* Release both lines immediately (end of session). */
void target_release(void);
