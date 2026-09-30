/* User scripts in Berry (https://github.com/berry-lang/berry).
 *
 * One script, stored in the "storage" partition, runs in its own task on
 * the second core with:
 *   - a memory budget (allocations beyond it fail as a script error),
 *   - a time limit per run of the script's code (a runaway loop is stopped),
 *   - a crash guard: if the device resets abnormally while script code is
 *     running, autostart is switched off.
 *
 * Script API (on top of the Berry standard modules string, json, math,
 * time, global, gc, introspect, strict):
 *   print(...)              -> script console (page and UART log)
 *   millis()                -> ms since boot
 *   every(ms, fn) -> id     repeat fn every ms
 *   after(ms, fn) -> id     run fn once after ms
 *   cancel(id)
 *   output(key, value)      result shown on the page (and later the display);
 *   output(key)             removes it
 *   status()                -> map with the device status (application data)
 *   heap()                  -> free heap of the device, bytes
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    size_t mem_limit;           /* bytes for the script's allocations */
    uint32_t time_limit_ms;     /* one run of script code (top level or a timer) */
    /* Appends a JSON object with the device status (for status()); may be NULL. */
    size_t (*status_json)(char *buf, size_t pos, size_t cap);
    const char *example;        /* shown in the editor while nothing is saved */
} script_config_t;

typedef enum {
    SCRIPT_STOPPED,
    SCRIPT_RUNNING,
    SCRIPT_ERROR,               /* stopped by an error; see the message */
} script_state_t;

esp_err_t script_start(const script_config_t *cfg);

/* Control, executed by the script task. */
esp_err_t script_run(void);
esp_err_t script_stop(void);

/* Stored script. script_save() stops the script and replaces the text
 * (done by the script task; waits for it). script_read() reads the saved
 * text in chunks. */
esp_err_t script_save(const char *src, size_t len);
size_t script_saved_len(void);
esp_err_t script_read(size_t offset, void *buf, size_t len);
size_t script_max_len(void);

esp_err_t script_set_autostart(bool on);

/* The n-th current output (0-based, in slot order), for the display.
 * Returns false when there are fewer outputs. */
bool script_output_get(int n, char *key, size_t key_len, char *val, size_t val_len);

/* JSON for the page: state, error, memory, outputs, console lines after
 * `since` (sequence number). Returns new pos. */
size_t script_state_json(char *buf, size_t pos, size_t cap, uint32_t since);

/* Registers /api/script* and /script.js on the setup portal. */
esp_err_t script_web_start(void);
