#ifndef ENGINE_LOG_H
#define ENGINE_LOG_H

#include <stddef.h>
#include <stdint.h>

enum {
    ENGINE_LOG_ERROR = 0,
    ENGINE_LOG_WARN  = 1,
    ENGINE_LOG_INFO  = 2,
    ENGINE_LOG_DEBUG = 3
};

// Open the diagnostic log.  path == NULL (or a path that cannot be opened)
// falls back to stderr so PC tests still see output.
void engine_log_init(const char *path);
void engine_log_close(void);

// Set minimum level that actually gets written (default ENGINE_LOG_INFO).
void engine_log_set_level(int level);

void engine_log(int level, const char *fmt, ...);

// Watchdog ticks: each engine thread stores its tick counter here once per
// loop iteration. A PC-only watchdog thread (engine.c) samples the array and
// writes the per-thread ages to watchdog.log, so a hung process can be
// diagnosed without a debugger. Cheap lock-free stores on Switch too.
// Indices: 0=dht 1=discovery 2=listener 3=upnp 4=netloop 5=writer 6=reader 7=watchdog
void tsnx_engine_wd_tick(int idx);
uint64_t tsnx_engine_wd_last(int idx);   // last tick of thread idx (0 = never)

#endif
