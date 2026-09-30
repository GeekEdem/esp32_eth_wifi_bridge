/* Berry configuration for ESP32 without PSRAM (WT32-ETH01).
 * Based on berry/default/berry_conf.h; trimmed for RAM and flash. */
#ifndef BERRY_CONF_H
#define BERRY_CONF_H

#include <assert.h>
#include <stddef.h>

#ifndef BE_DEBUG
#define BE_DEBUG                        0
#endif

#define BE_INTGER_TYPE                  0       /* int (32-bit) */
#define BE_USE_SINGLE_FLOAT             1       /* the ESP32 FPU is single precision */
#define BE_BYTES_MAX_SIZE               (8*1024)
#define BE_USE_PRECOMPILED_OBJECT       1       /* constant tables in flash */

/* Line numbers in error messages, but no local variable names. */
#define BE_DEBUG_SOURCE_FILE            1
#define BE_DEBUG_RUNTIME_INFO           1
#define BE_DEBUG_VAR_INFO               0

/* The observability hook stops runaway scripts (every 2^18 instructions). */
#define BE_USE_PERF_COUNTERS            1
#define BE_VM_OBSERVABILITY_SAMPLING    18

#define BE_STACK_TOTAL_MAX              2000
#define BE_STACK_FREE_MIN               10
#define BE_STACK_START                  50
#define BE_CONST_SEARCH_SIZE            50
#define BE_USE_STR_HASH_CACHE           0

/* No file system: the script comes from the "storage" partition. */
#define BE_USE_FILE_SYSTEM              0
#define BE_USE_SCRIPT_COMPILER          1
#define BE_USE_BYTECODE_SAVER           0
#define BE_USE_BYTECODE_LOADER          0
#define BE_USE_SHARED_LIB               0
#define BE_USE_OVERLOAD_HASH            1
#define BE_MAX_PARSER_DEPTH             25      /* fits the 8 KB task stack */

#define BE_USE_DEBUG_HOOK               0
#define BE_USE_DEBUG_GC                 0
#define BE_USE_DEBUG_STACK              0
#define BE_USE_MEM_ALIGNED              0

#define BE_USE_STRING_MODULE            1
#define BE_USE_JSON_MODULE              1
#define BE_USE_MATH_MODULE              1
#define BE_USE_TIME_MODULE              1
#define BE_USE_OS_MODULE                0
#define BE_USE_GLOBAL_MODULE            1
#define BE_USE_SYS_MODULE               0
#define BE_USE_DEBUG_MODULE             0
#define BE_USE_GC_MODULE                1
#define BE_USE_SOLIDIFY_MODULE          0
#define BE_USE_INTROSPECT_MODULE        1
#define BE_USE_STRICT_MODULE            1

/* Allocations go through a budget (see script_runtime.c); running out
 * raises a memory error in the script instead of starving the firmware. */
void *script_be_malloc(size_t size);
void script_be_free(void *ptr);
void *script_be_realloc(void *ptr, size_t size);
void script_be_abort(void);

#define BE_EXPLICIT_ABORT               script_be_abort
#define BE_EXPLICIT_EXIT(x)             script_be_abort()
#define BE_EXPLICIT_MALLOC              script_be_malloc
#define BE_EXPLICIT_FREE                script_be_free
#define BE_EXPLICIT_REALLOC             script_be_realloc

#define be_assert(expr)                 assert(expr)

#endif
