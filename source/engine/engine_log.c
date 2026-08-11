#include "engine_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <switch.h>

static Mutex g_log_mtx;
static FILE *g_log_f = NULL;
static int g_log_level = ENGINE_LOG_INFO;
static int g_log_init = 0;
static int g_log_first_open = 1;

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
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    const char *lvl = (level == ENGINE_LOG_ERROR) ? "ERR" :
                      (level == ENGINE_LOG_WARN)  ? "WRN" :
                      (level == ENGINE_LOG_INFO)  ? "INF" : "DBG";

    fprintf(g_log_f, "[%s %s] ", ts, lvl);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_f, fmt, ap);
    va_end(ap);
    fprintf(g_log_f, "\n");
    fflush(g_log_f);
    mutexUnlock(&g_log_mtx);
}
