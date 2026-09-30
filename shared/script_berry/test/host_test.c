/* Host test of the Berry side of the runtime: our berry_conf.h, prelude.be
 * (timers, status()), the WT32 example script, the runaway-loop stop via the
 * observability hook, the memory budget and syntax error reporting.
 * The FreeRTOS task, flash storage and web routes are not covered here.
 *
 *   ./run_host_test.sh
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "berry.h"

/* --- same budget semantics as script_runtime.c --- */
typedef struct { size_t size; } mhdr_t;
static size_t mem_used, mem_limit = 40 * 1024;
void *script_be_realloc(void *ptr, size_t size);
void *script_be_malloc(size_t s) { return script_be_realloc(NULL, s); }
void script_be_free(void *p) { if (!p) return; mhdr_t *h = (mhdr_t *)p - 1; mem_used -= h->size; free(h); }
void *script_be_realloc(void *ptr, size_t size)
{
    mhdr_t *old = ptr ? (mhdr_t *)ptr - 1 : NULL;
    size_t old_size = old ? old->size : 0;
    if (size == 0) { script_be_free(ptr); return NULL; }
    if (size > old_size && mem_used - old_size + size > mem_limit) return NULL;
    mhdr_t *h = realloc(old, sizeof(mhdr_t) + size);
    if (!h) return NULL;
    mem_used = mem_used - old_size + size;
    h->size = size;
    return h + 1;
}
void script_be_abort(void) { abort(); }

static char console[8192];
void script_console_write(const char *b, size_t l) { strncat(console, b, l < sizeof(console) - strlen(console) - 1 ? l : 0); }

/* --- natives as in script_runtime.c, with a fake clock --- */
static long fake_ms;
static char outputs[16][2][64];
static int n_millis(bvm *vm) { be_pushint(vm, (bint)fake_ms); be_return(vm); }
static int n_heap(bvm *vm) { be_pushint(vm, 120000); be_return(vm); }
static int n_output(bvm *vm)
{
    const char *k = be_tostring(vm, 1);
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!strcmp(outputs[i][0], k)) slot = i;
    if (slot < 0) for (int i = 0; i < 16; i++) if (!outputs[i][0][0]) { slot = i; break; }
    if (be_top(vm) < 2 || be_isnil(vm, 2)) { if (slot >= 0) outputs[slot][0][0] = 0; be_return_nil(vm); }
    snprintf(outputs[slot][0], 64, "%s", k);
    snprintf(outputs[slot][1], 64, "%s", be_tostring(vm, 2));
    be_return_nil(vm);
}
static const char *status_json = "{\"mode\":\"client\",\"eth\":true,\"devIp\":\"192.168.1.50\",\"devMac\":\"00:11:22:33:44:55\"}";
static int n_status_json(bvm *vm) { be_pushstring(vm, status_json); be_return(vm); }
static const char *out(const char *k) { for (int i = 0; i < 16; i++) if (!strcmp(outputs[i][0], k)) return outputs[i][1]; return NULL; }

static clock_t call_start; static double limit_s = 0.3;
static void obs_hook(bvm *vm, int event, ...)
{
    if (event == BE_OBS_VM_HEARTBEAT && (double)(clock() - call_start) / CLOCKS_PER_SEC > limit_s)
        be_raise(vm, "timeout_error", "script code ran too long");
}

static char err[256];
static bool pcall(bvm *vm, int argc)
{
    call_start = clock();
    int r = be_pcall(vm, argc);
    if (r == BE_EXCEPTION) { snprintf(err, sizeof err, "%s: %s", be_tostring(vm, -2), be_tostring(vm, -1)); be_pop(vm, 2 + argc + 1); return false; }
    if (r == BE_MALLOC_FAIL) { snprintf(err, sizeof err, "memory_error: script memory limit reached"); be_pop(vm, argc + 1); return false; }
    if (r) { snprintf(err, sizeof err, "error %d", r); be_pop(vm, argc + 1); return false; }
    return true;
}
static bool load_run(bvm *vm, const char *name, const char *src)
{
    int r = be_loadbuffer(vm, name, src, strlen(src));
    if (r == BE_EXCEPTION) { snprintf(err, sizeof err, "%s: %s", be_tostring(vm, -2), be_tostring(vm, -1)); be_pop(vm, 2); return false; }
    if (r) { snprintf(err, sizeof err, "load %d", r); return false; }
    bool ok = pcall(vm, 0);
    if (ok) be_pop(vm, 1);
    return ok;
}
static char *slurp(const char *path) { FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); char *b = malloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) abort(); b[n] = 0; fclose(f); return b; }

static bvm *new_vm(const char *prelude)
{
    bvm *vm = be_vm_new();
    be_set_obs_hook(vm, obs_hook);
    be_regfunc(vm, "millis", n_millis); be_regfunc(vm, "heap", n_heap);
    be_regfunc(vm, "output", n_output); be_regfunc(vm, "_status_json", n_status_json);
    err[0] = 0;
    assert(load_run(vm, "prelude", prelude) || (fprintf(stderr, "prelude: %s\n", err), 0));
    return vm;
}
static int run_timers(bvm *vm)
{
    be_getglobal(vm, "_run_timers");
    if (!pcall(vm, 0)) return -2;
    int w = be_toint(vm, -1); be_pop(vm, 1); return w;
}

int main(int argc, char **argv)
{
    char *prelude = slurp(argv[1]);
    char *example = slurp(argv[2]);

    /* 1. example script: outputs from status(), every(5000) re-runs */
    bvm *vm = new_vm(prelude);
    assert(load_run(vm, "script", example) || (fprintf(stderr, "example: %s\n", err), 0));
    assert(strstr(console, "скрипт запущено"));
    assert(out("Режим") && !strcmp(out("Режим"), "клієнт"));
    assert(!strcmp(out("IP пристрою"), "192.168.1.50"));
    assert(!strcmp(out("Вільна памʼять"), "117 КБ"));
    assert(run_timers(vm) == 5000);
    status_json = "{\"mode\":\"own\",\"eth\":false,\"devIp\":\"\",\"devMac\":\"\"}";
    fake_ms = 4999; assert(run_timers(vm) == 1); assert(!strcmp(out("Режим"), "клієнт"));
    fake_ms = 5000; assert(run_timers(vm) == 5000);
    assert(!strcmp(out("Режим"), "роутер") && !strcmp(out("Ethernet"), "немає") && !strcmp(out("IP пристрою"), "—"));
    status_json = "{\"mode\":\"ap\",\"eth\":true,\"devIp\":\"\",\"devMac\":\"\"}";
    fake_ms = 10000; assert(run_timers(vm) == 5000);
    assert(!strcmp(out("Режим"), "точка доступу") && !strcmp(out("Ethernet"), "є лінк"));
    be_vm_delete(vm);
    assert(mem_used == 0);

    /* 2. after / cancel */
    fake_ms = 0; vm = new_vm(prelude);
    assert(load_run(vm, "t", "var n = 0 var id = every(100, def () n += 1 end) after(250, def () cancel(id) output('n', n) end)"));
    for (fake_ms = 0; fake_ms <= 1000; fake_ms += 50) { int w = run_timers(vm); assert(w >= -1); }
    assert(out("n") && !strcmp(out("n"), "2"));
    assert(run_timers(vm) == -1);
    be_vm_delete(vm);

    /* 3. runaway loop is stopped by the hook; the VM stays usable */
    vm = new_vm(prelude);
    assert(!load_run(vm, "loop", "while true end"));
    assert(strstr(err, "timeout_error"));
    assert(load_run(vm, "after", "output('alive', 1)") && !strcmp(out("alive"), "1"));
    be_vm_delete(vm);

    /* 4. memory budget: a runaway list gets a memory error, not a crash */
    vm = new_vm(prelude);
    assert(!load_run(vm, "mem", "var l = [] while true l.push('x' + str(size(l))) end"));
    assert(strstr(err, "memory_error") || (fprintf(stderr, "mem: %s\n", err), 0));
    be_vm_delete(vm);                   /* the runtime drops the VM after an error */
    assert(mem_used == 0);
    vm = new_vm(prelude);
    assert(load_run(vm, "fresh", "output('ok', 'yes')") && !strcmp(out("ok"), "yes"));
    be_vm_delete(vm);

    /* 5. syntax error with the line number */
    vm = new_vm(prelude);
    assert(!load_run(vm, "script", "var a = 1\nvar b = (2\n"));
    assert(strstr(err, "syntax_error") && strstr(err, "script:"));
    printf("syntax error reads: %s\n", err);
    be_vm_delete(vm);

    free(prelude);
    free(example);
    printf("all Berry host tests passed (budget %zu B)\n", mem_limit);
    return 0;
}
