#include "engine_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

static Mutex g_log_mtx;
static FILE *g_log_f = NULL;
static int g_log_level = ENGINE_LOG_INFO;
static int g_log_init = 0;
static int g_log_first_open = 1;

// The engine's hot paths (netloop dials, piece arrivals) must not pay for
// time()/localtime(): on Switch time() is an IPC to the time:u service and
// strftime/fflush hit the fs service, so a dial storm used to serialize those
// on the netloop thread. Timestamps come from the system tick instead: the
// epoch is captured once at init, the wall clock is derived from tick deltas,
// and the formatted date string is cached until the second changes.
static time_t g_epoch_secs;
static u64 g_epoch_tick;
static int g_cached_second = -1;
static char g_cached_ts[32];

static time_t wall_seconds(void) {
    return g_epoch_secs +
           (time_t)((armGetSystemTick() - g_epoch_tick) / armGetSystemTickFreq());
}

void engine_log_init(const char *path) {
    if (g_log_init) return;
    mutexInit(&g_log_mtx);
    if (path) {
        // Make sure the containing directory exists before opening the file.
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0777);
        }
        // Truncate on the very first open of the process so engine.log reflects
        // only the current session; reopen after a stop appends.
        g_log_f = fopen(path, g_log_first_open ? "w" : "a");
        g_log_first_open = 0;
        if (!g_log_f) g_log_f = stderr;
    } else {
        g_log_f = stderr;
    }
    g_epoch_secs = time(NULL);          // one IPC, once
    g_epoch_tick = armGetSystemTick();
    g_cached_second = -1;
    g_log_init = 1;
}

void engine_log_close(void) {
    if (!g_log_init) return;
    if (g_log_f && g_log_f != stderr) {
        fclose(g_log_f);
    }
    g_log_f = NULL;
    g_log_init = 0;
}

void engine_log_set_level(int level) {
    if (level < ENGINE_LOG_ERROR) level = ENGINE_LOG_ERROR;
    if (level > ENGINE_LOG_DEBUG) level = ENGINE_LOG_DEBUG;
    g_log_level = level;
}

void engine_log(int level, const char *fmt, ...) {
    if (!g_log_init || level > g_log_level || !g_log_f) return;

    mutexLock(&g_log_mtx);
    time_t now = wall_seconds();
    if (now != g_cached_second) {
        g_cached_second = (int)now;
        struct tm *tm = localtime(&now);
        strftime(g_cached_ts, sizeof(g_cached_ts), "%Y-%m-%d %H:%M:%S", tm);
    }
    const char *lvl = (level == ENGINE_LOG_ERROR) ? "ERR" :
                      (level == ENGINE_LOG_WARN)  ? "WRN" :
                      (level == ENGINE_LOG_INFO)  ? "INF" : "DBG";

    fprintf(g_log_f, "[%s %s] ", g_cached_ts, lvl);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_f, fmt, ap);
    va_end(ap);
    fprintf(g_log_f, "\n");
    // fflush per line is an fs IPC on every call; only errors need to be
    // visible right away. Buffered lines are still written out on close (or
    // when the 4 KB stdio buffer fills).
    if (level <= ENGINE_LOG_WARN) fflush(g_log_f);
    mutexUnlock(&g_log_mtx);
}

//-----------------------------------------------------------------------------
// Watchdog ticks (see engine_log.h)
//-----------------------------------------------------------------------------
static volatile u64 g_wd_tick[8];

void tsnx_engine_wd_tick(int idx) {
    if (idx >= 0 && idx < 8)
        g_wd_tick[idx] = armGetSystemTick();
}

// Last tick value of thread `idx`, for the PC watchdog sampler (or 0 if the
// thread never ticked).
u64 tsnx_engine_wd_last(int idx) {
    if (idx < 0 || idx >= 8) return 0;
    return (u64)g_wd_tick[idx];
}
