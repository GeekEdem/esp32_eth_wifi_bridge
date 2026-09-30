/* Berry script runtime: VM task, limits, console, outputs. See script.h. */
#include "script.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "berry.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "script_internal.h"
#include "setup_portal.h"

#define NVS_NS          "script"
#define TASK_STACK      10240
#define TASK_PRIO       2
#define TASK_CORE       1
#define IDLE_WAIT_MS    1000
#define CRASH_MAGIC     0x5C817A3Du

static const char *TAG = "script";

extern const char prelude_start[] asm("_binary_prelude_be_start");
extern const char prelude_end[] asm("_binary_prelude_be_end");

typedef enum { CMD_RUN, CMD_STOP, CMD_SAVE } cmd_type_t;

typedef struct {
    cmd_type_t type;
    const char *src;            /* CMD_SAVE */
    size_t len;
} cmd_t;

static script_config_t s_cfg;
static QueueHandle_t s_cmds;
static SemaphoreHandle_t s_save_done;
static esp_err_t s_save_result;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bvm *s_vm;
static volatile script_state_t s_state;
static char s_error[160];
static bool s_autostart;
static bool s_crash_disabled;
static uint32_t s_runs;
static int64_t s_call_start_us;

/* Set while script code runs; survives a software reset but not power-off. */
static RTC_NOINIT_ATTR uint32_t s_in_script;

/* ---------- memory budget (used by Berry through berry_conf.h) ---------- */

typedef struct {
    size_t size;
} mhdr_t;

static size_t s_mem_used;
static size_t s_mem_peak;

void *script_be_malloc(size_t size)
{
    return script_be_realloc(NULL, size);
}

void script_be_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    mhdr_t *h = (mhdr_t *)ptr - 1;
    s_mem_used -= h->size;
    free(h);
}

void *script_be_realloc(void *ptr, size_t size)
{
    mhdr_t *old = ptr ? (mhdr_t *)ptr - 1 : NULL;
    size_t old_size = old ? old->size : 0;
    if (size == 0) {
        script_be_free(ptr);
        return NULL;
    }
    if (size > old_size && s_mem_used - old_size + size > s_cfg.mem_limit) {
        return NULL;                /* Berry collects garbage, retries, then raises */
    }
    mhdr_t *h = realloc(old, sizeof(mhdr_t) + size);
    if (!h) {
        return NULL;
    }
    s_mem_used = s_mem_used - old_size + size;
    if (s_mem_used > s_mem_peak) {
        s_mem_peak = s_mem_used;
    }
    h->size = size;
    return h + 1;
}

void script_be_abort(void)
{
    ESP_LOGE(TAG, "Berry fatal error outside a protected call");
    abort();
}

/* ---------- console and outputs ---------- */

#define CONSOLE_LINES   40
#define CONSOLE_LINE    120
#define MAX_OUTPUTS     16
#define OUT_KEY         24
#define OUT_VAL         64

static char s_console[CONSOLE_LINES][CONSOLE_LINE];
static uint32_t s_console_seq;          /* sequence number of the next line */
static char s_partial[CONSOLE_LINE];
static size_t s_partial_len;

static struct {
    char key[OUT_KEY];
    char val[OUT_VAL];
} s_out[MAX_OUTPUTS];

static void console_push_line(const char *line)
{
    ESP_LOGI(TAG, "%s", line);
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_console[s_console_seq % CONSOLE_LINES], line, CONSOLE_LINE);
    s_console_seq++;
    portEXIT_CRITICAL(&s_lock);
}

void script_console_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || s_partial_len == CONSOLE_LINE - 1) {
            s_partial[s_partial_len] = '\0';
            console_push_line(s_partial);
            s_partial_len = 0;
            if (c == '\n') {
                continue;
            }
        }
        s_partial[s_partial_len++] = c;
    }
}

void script_console_printf(const char *fmt, ...)
{
    char line[CONSOLE_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    console_push_line(line);
}

bool script_output_get(int n, char *key, size_t key_len, char *val, size_t val_len)
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (s_out[i].key[0] && n-- == 0) {
            strlcpy(key, s_out[i].key, key_len);
            strlcpy(val, s_out[i].val, val_len);
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static void outputs_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_out, 0, sizeof(s_out));
    portEXIT_CRITICAL(&s_lock);
}

/* ---------- natives ---------- */

static int n_millis(bvm *vm)
{
    be_pushint(vm, (bint)(esp_timer_get_time() / 1000));
    be_return(vm);
}

static int n_heap(bvm *vm)
{
    be_pushint(vm, (bint)esp_get_free_heap_size());
    be_return(vm);
}

static int n_output(bvm *vm)
{
    int argc = be_top(vm);
    if (argc < 1 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "output(key [, value])");
    }
    char key[OUT_KEY];
    strlcpy(key, be_tostring(vm, 1), sizeof(key));
    bool remove = argc < 2 || be_isnil(vm, 2);
    char val[OUT_VAL] = "";
    if (!remove) {
        strlcpy(val, be_tostring(vm, 2), sizeof(val));  /* converts numbers etc. */
    }
    portENTER_CRITICAL(&s_lock);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (s_out[i].key[0] && strcmp(s_out[i].key, key) == 0) {
            slot = i;
        } else if (!s_out[i].key[0] && free_slot < 0) {
            free_slot = i;
        }
    }
    bool full = false;
    if (remove) {
        if (slot >= 0) {
            s_out[slot].key[0] = '\0';
        }
    } else {
        if (slot < 0) {
            slot = free_slot;
        }
        if (slot >= 0) {
            strlcpy(s_out[slot].key, key, OUT_KEY);
            strlcpy(s_out[slot].val, val, OUT_VAL);
        } else {
            full = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (full) {
        be_raise(vm, "range_error", "too many outputs (16)");
    }
    be_return_nil(vm);
}

static int n_status_json(bvm *vm)
{
    size_t cap = 1536;
    char *buf = malloc(cap);        /* outside the script budget: short-lived */
    if (!buf) {
        be_raise(vm, "memory_error", "status");
    }
    size_t pos = setup_portal_appendf(buf, 0, cap, "{");
    if (s_cfg.status_json) {
        size_t start = pos;
        pos = s_cfg.status_json(buf, pos, cap - 2);
        if (pos > start && buf[start] == ',') {
            memmove(buf + start, buf + start + 1, pos - start - 1);   /* drop the leading comma */
            pos--;
        }
    }
    setup_portal_appendf(buf, pos, cap, "}");
    be_pushstring(vm, buf);
    free(buf);
    be_return(vm);
}

/* ---------- running script code ---------- */

static void obs_hook(bvm *vm, int event, ...)
{
    if (event == BE_OBS_VM_HEARTBEAT &&
        esp_timer_get_time() - s_call_start_us > (int64_t)s_cfg.time_limit_ms * 1000) {
        be_raise(vm, "timeout_error", "script code ran too long");
    }
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
    s_state = SCRIPT_ERROR;
    script_console_printf("! %s", s_error);
}

/* Calls the function on top of the stack with argc args under the limits.
 * On error records it and returns false (the stack is cleaned up). */
static bool guarded_pcall(int argc)
{
    s_call_start_us = esp_timer_get_time();
    s_in_script = CRASH_MAGIC;
    int r = be_pcall(s_vm, argc);
    s_in_script = 0;
    if (r != BE_OK) {
        if (r == BE_EXCEPTION) {
            set_error("%s: %s", be_tostring(s_vm, -2), be_tostring(s_vm, -1));
            be_pop(s_vm, 2);
        } else if (r == BE_MALLOC_FAIL) {
            set_error("memory_error: script memory limit reached (%u KB)", (unsigned)(s_cfg.mem_limit / 1024));
        } else {
            set_error("error %d", r);
        }
        be_pop(s_vm, argc + 1);
        return false;
    }
    return true;
}

static bool load_and_run(const char *name, const char *src, size_t len)
{
    s_call_start_us = esp_timer_get_time();
    s_in_script = CRASH_MAGIC;
    int r = be_loadbuffer(s_vm, name, src, len);
    s_in_script = 0;
    if (r != BE_OK) {
        if (r == BE_EXCEPTION) {                   /* syntax error: type, message with line */
            set_error("%s: %s", be_tostring(s_vm, -2), be_tostring(s_vm, -1));
            be_pop(s_vm, 2);
        } else if (r == BE_MALLOC_FAIL) {
            set_error("memory_error: script memory limit reached while compiling (%u KB)",
                      (unsigned)(s_cfg.mem_limit / 1024));
        } else {
            set_error("load error %d", r);
        }
        return false;
    }
    bool ok = guarded_pcall(0);
    if (ok) {
        be_pop(s_vm, 1);
    }
    return ok;
}

static void vm_destroy(void)
{
    if (s_vm) {
        be_vm_delete(s_vm);
        s_vm = NULL;
    }
    if (s_partial_len) {
        script_console_write("\n", 1);
    }
}

static void vm_start(void)
{
    vm_destroy();
    outputs_clear();
    s_error[0] = '\0';
    s_mem_peak = s_mem_used;

    size_t len;
    const char *src = script_store_map(&len);
    if (!len) {
        s_state = SCRIPT_STOPPED;
        script_console_printf("-- no script saved");
        return;
    }
    s_vm = be_vm_new();
    if (!s_vm) {
        set_error("not enough memory for the VM");
        return;
    }
    be_set_obs_hook(s_vm, obs_hook);
    be_regfunc(s_vm, "millis", n_millis);
    be_regfunc(s_vm, "heap", n_heap);
    be_regfunc(s_vm, "output", n_output);
    be_regfunc(s_vm, "_status_json", n_status_json);

    s_runs++;
    s_state = SCRIPT_RUNNING;
    script_console_printf("-- start #%lu", (unsigned long)s_runs);
    if (!load_and_run("prelude", prelude_start, prelude_end - prelude_start - 1) ||
        !load_and_run("script", src, len)) {
        vm_destroy();
    }
}

/* Runs due timers; returns ms to wait (IDLE_WAIT_MS when nothing is scheduled). */
static uint32_t run_timers(void)
{
    if (!s_vm || s_state != SCRIPT_RUNNING) {
        return IDLE_WAIT_MS;
    }
    if (!be_getglobal(s_vm, "_run_timers")) {
        be_pop(s_vm, 1);
        return IDLE_WAIT_MS;
    }
    if (!guarded_pcall(0)) {
        vm_destroy();
        return IDLE_WAIT_MS;
    }
    int wait = be_isint(s_vm, -1) ? be_toint(s_vm, -1) : -1;
    be_pop(s_vm, 1);
    if (wait < 0 || wait > IDLE_WAIT_MS) {
        return IDLE_WAIT_MS;
    }
    return wait;
}

static void script_task(void *arg)
{
    if (s_autostart && !s_crash_disabled) {
        vm_start();
    }
    uint32_t wait = IDLE_WAIT_MS;
    while (true) {
        cmd_t cmd;
        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(wait ? wait : 1)) == pdTRUE) {
            if (cmd.type == CMD_RUN) {
                s_crash_disabled = false;
                vm_start();
            } else {
                if (s_vm) {
                    vm_destroy();
                    script_console_printf("-- stopped");
                }
                s_state = SCRIPT_STOPPED;
                if (cmd.type == CMD_SAVE) {
                    s_save_result = script_store_write(cmd.src, cmd.len);
                    xSemaphoreGive(s_save_done);
                }
            }
        }
        wait = run_timers();
    }
}

/* ---------- public ---------- */

esp_err_t script_run(void)
{
    cmd_t c = { .type = CMD_RUN };
    return xQueueSend(s_cmds, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t script_stop(void)
{
    cmd_t c = { .type = CMD_STOP };
    return xQueueSend(s_cmds, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t script_save(const char *src, size_t len)
{
    if (len > script_max_len()) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_save_done, 0);             /* drop a stale signal */
    cmd_t c = { .type = CMD_SAVE, .src = src, .len = len };
    if (xQueueSend(s_cmds, &c, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* The script's current run is bounded by the time limit. */
    if (xSemaphoreTake(s_save_done, pdMS_TO_TICKS(s_cfg.time_limit_ms + 10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_save_result;
}

static esp_err_t nvs_set_flag(const char *key, bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, key, on);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t script_set_autostart(bool on)
{
    esp_err_t err = nvs_set_flag("autostart", on);
    if (err == ESP_OK) {
        s_autostart = on;
        if (on) {
            s_crash_disabled = false;
        }
    }
    return err;
}

size_t script_state_json(char *buf, size_t pos, size_t cap, uint32_t since)
{
    static const char *names[] = {
        [SCRIPT_STOPPED] = "stopped",
        [SCRIPT_RUNNING] = "running",
        [SCRIPT_ERROR] = "error",
    };
    size_t src_len = script_saved_len();
    pos = setup_portal_appendf(buf, pos, cap,
        "\"state\":\"%s\",\"autostart\":%s,\"crashDisabled\":%s,\"runs\":%lu,"
        "\"mem\":{\"used\":%u,\"peak\":%u,\"limit\":%u},\"heap\":%lu,\"saved\":%u,\"maxLen\":%u,\"error\":",
        names[s_state], s_autostart ? "true" : "false", s_crash_disabled ? "true" : "false",
        (unsigned long)s_runs, (unsigned)s_mem_used, (unsigned)s_mem_peak, (unsigned)s_cfg.mem_limit,
        (unsigned long)esp_get_free_heap_size(), (unsigned)src_len, (unsigned)script_max_len());
    pos = setup_portal_json_str(buf, pos, cap, s_error);

    pos = setup_portal_appendf(buf, pos, cap, ",\"outputs\":[");
    bool first = true;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        char key[OUT_KEY], val[OUT_VAL];
        portENTER_CRITICAL(&s_lock);
        strlcpy(key, s_out[i].key, sizeof(key));
        strlcpy(val, s_out[i].val, sizeof(val));
        portEXIT_CRITICAL(&s_lock);
        if (!key[0]) {
            continue;
        }
        pos = setup_portal_appendf(buf, pos, cap, "%s[", first ? "" : ",");
        pos = setup_portal_json_str(buf, pos, cap, key);
        pos = setup_portal_appendf(buf, pos, cap, ",");
        pos = setup_portal_json_str(buf, pos, cap, val);
        pos = setup_portal_appendf(buf, pos, cap, "]");
        first = false;
    }

    uint32_t seq;
    portENTER_CRITICAL(&s_lock);
    seq = s_console_seq;
    portEXIT_CRITICAL(&s_lock);
    uint32_t oldest = seq > CONSOLE_LINES ? seq - CONSOLE_LINES : 0;
    if (since < oldest) {
        since = oldest;
    }
    pos = setup_portal_appendf(buf, pos, cap, "],\"seq\":%lu,\"console\":[", (unsigned long)seq);
    for (uint32_t n = since; n < seq; n++) {
        char line[CONSOLE_LINE];
        portENTER_CRITICAL(&s_lock);
        strlcpy(line, s_console[n % CONSOLE_LINES], sizeof(line));
        portEXIT_CRITICAL(&s_lock);
        pos = setup_portal_appendf(buf, pos, cap, n == since ? "" : ",");
        pos = setup_portal_json_str(buf, pos, cap, line);
    }
    return setup_portal_appendf(buf, pos, cap, "]");
}

const char *script_example(void)
{
    return s_cfg.example ? s_cfg.example : "";
}

esp_err_t script_start(const script_config_t *cfg)
{
    s_cfg = *cfg;
    esp_err_t err = script_store_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no script storage (%s)", esp_err_to_name(err));
    }

    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "autostart", &v) == ESP_OK) {
            s_autostart = v;
        }
        nvs_close(h);
    }

    /* A reset while script code was running (panic, watchdog) keeps the
     * next boot from starting the script again. */
    esp_reset_reason_t why = esp_reset_reason();
    bool abnormal = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT ||
                    why == ESP_RST_WDT;
    if (abnormal && s_in_script == CRASH_MAGIC && s_autostart) {
        s_crash_disabled = true;
        script_set_autostart(false);
        s_crash_disabled = true;
        ESP_LOGE(TAG, "reset while the script was running: autostart off");
        script_console_printf("! the device reset while the script was running; autostart is off");
    }
    s_in_script = 0;

    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    s_save_done = xSemaphoreCreateBinary();
    if (!s_cmds || !s_save_done) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(script_task, "script", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
