#include "engine_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "thread_cfg.h"

// Asynchronous logger. engine_log() formats the line on the calling thread and
// appends it to an in-memory ring; a writer thread on the work core drains the
// ring to the file in batches. The netloop, writer and reader threads therefore
// never block on the SD card (every fwrite/fflush there is an IPC to the fs
// service, and a stdio buffer filling up used to land that write right inside
// the netloop). Lines are on disk within LOG_FLUSH_MS; WARN/ERR wake the
// writer at once. When the ring is full new lines are dropped and counted --
// the caller never waits.
//
// Before engine_log_init() (and after close, or if the writer thread could not
// start) lines are written synchronously, as before.

#define LOG_RING_SIZE   (256 * 1024)
#define LOG_LINE_MAX    1024
#define LOG_FLUSH_MS    500
#define LOG_BATCH_MAX   (64 * 1024)

static Mutex g_log_mtx;
static CondVar g_log_cv;
static FILE *g_log_f = NULL;
static int g_log_level = ENGINE_LOG_INFO;
static int g_log_init = 0;
static int g_log_first_open = 1;

static char g_ring[LOG_RING_SIZE];
static size_t g_ring_head = 0;   // next byte to write out
static size_t g_ring_len = 0;    // bytes queued
static unsigned g_dropped = 0;   // lines lost to a full ring since the last report
static Thread g_writer;
static volatile int g_writer_running = 0;
static volatile int g_writer_stop = 0;
static volatile int g_writer_busy = 0;   // holds a batch taken from the ring
static char g_batch[LOG_BATCH_MAX];      // writer-private

// The engine's hot paths (netloop dials, piece arrivals) must not pay for
// time()/localtime(): on Switch time() is an IPC to the time:u service. The
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

// Caller holds g_log_mtx.
static void ring_put(const char *p, size_t n) {
    size_t tail = (g_ring_head + g_ring_len) % LOG_RING_SIZE;
    size_t first = LOG_RING_SIZE - tail;
    if (first > n) first = n;
    memcpy(g_ring + tail, p, first);
    if (n > first) memcpy(g_ring, p + first, n - first);
    g_ring_len += n;
}

// Caller holds g_log_mtx. Moves up to `cap` queued bytes into `dst`.
static size_t ring_take(char *dst, size_t cap) {
    size_t n = g_ring_len < cap ? g_ring_len : cap;
    size_t first = LOG_RING_SIZE - g_ring_head;
    if (first > n) first = n;
    memcpy(dst, g_ring + g_ring_head, first);
    if (n > first) memcpy(dst + first, g_ring, n - first);
    g_ring_head = (g_ring_head + n) % LOG_RING_SIZE;
    g_ring_len -= n;
    return n;
}

static void log_writer_main(void *arg) {
    (void)arg;
    const u64 flush_ticks = armGetSystemTickFreq() / 1000 * LOG_FLUSH_MS;
    u64 last_flush = armGetSystemTick();
    for (;;) {
        mutexLock(&g_log_mtx);
        if (g_ring_len == 0 && !g_writer_stop)
            condvarWaitTimeout(&g_log_cv, &g_log_mtx, (u64)LOG_FLUSH_MS * 1000000ULL);
        size_t n = ring_take(g_batch, sizeof(g_batch) - 64);
        unsigned dropped = g_dropped;
        g_dropped = 0;
        int stop = g_writer_stop;
        g_writer_busy = n > 0 || dropped > 0;
        FILE *f = g_log_f;
        mutexUnlock(&g_log_mtx);

        if (dropped > 0)
            n += (size_t)snprintf(g_batch + n, 64, "[log] dropped %u lines (ring full)\n", dropped);
        if (n > 0 && f) fwrite(g_batch, 1, n, f);
        u64 now = armGetSystemTick();
        if (f && now - last_flush >= flush_ticks) {
            fflush(f);
            last_flush = now;
        }
        g_writer_busy = 0;

        if (stop) {
            mutexLock(&g_log_mtx);
            int empty = g_ring_len == 0 && g_dropped == 0;
            mutexUnlock(&g_log_mtx);
            if (empty) {
                if (f) fflush(f);
                break;
            }
        }
    }
}

void engine_log_init(const char *path) {
    if (g_log_init) return;
    mutexInit(&g_log_mtx);
    condvarInit(&g_log_cv);
    if (path) {
        // Make sure the containing directory exists before opening the file.
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0777);
        }
        // Rotate previous session's log on the very first open of the process so a
        // post-crash restart does not wipe out the crash session's engine.log.
        if (g_log_first_open) {
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size > 0) {
                char prev_path[256];
                const char *dot = strrchr(path, '.');
                if (dot) {
                    size_t prefix_len = (size_t)(dot - path);
                    if (prefix_len > sizeof(prev_path) - 16) prefix_len = sizeof(prev_path) - 16;
                    snprintf(prev_path, sizeof(prev_path), "%.*s.prev%s", (int)prefix_len, path, dot);
                } else {
                    snprintf(prev_path, sizeof(prev_path), "%s.prev", path);
                }
                remove(prev_path);
                rename(path, prev_path);
            }
        }
        g_log_f = fopen(path, g_log_first_open ? "w" : "a");
        g_log_first_open = 0;
        if (!g_log_f) g_log_f = stderr;
    } else {
        g_log_f = stderr;
    }
    g_epoch_secs = time(NULL);          // one IPC, once
    g_epoch_tick = armGetSystemTick();
    g_cached_second = -1;
    g_ring_head = g_ring_len = 0;
    g_dropped = 0;
    g_writer_stop = 0;
    g_writer_running = 0;
    if (threadCreate(&g_writer, log_writer_main, NULL, NULL, 0x8000,
                     TSNX_THREAD_ARGS(TSNX_CORE_WORK, TSNX_PRIO_LOGGER)) == 0) {
        if (threadStart(&g_writer) == 0) g_writer_running = 1;
        else threadClose(&g_writer);
    }
    g_log_init = 1;
}

// Wait (bounded) until everything queued so far is in the file.
void engine_log_flush(void) {
    if (!g_log_init) return;
    if (g_writer_running) {
        mutexLock(&g_log_mtx);
        condvarWakeAll(&g_log_cv);
        mutexUnlock(&g_log_mtx);
        for (int i = 0; i < 100; i++) {   // <= ~200 ms
            mutexLock(&g_log_mtx);
            int empty = g_ring_len == 0;
            mutexUnlock(&g_log_mtx);
            if (empty && !g_writer_busy) break;
            svcSleepThread(2000000ULL);
        }
    }
    mutexLock(&g_log_mtx);
    if (g_log_f) fflush(g_log_f);
    mutexUnlock(&g_log_mtx);
}

void engine_log_close(void) {
    if (!g_log_init) return;
    if (g_writer_running) {
        mutexLock(&g_log_mtx);
        g_writer_stop = 1;
        condvarWakeAll(&g_log_cv);
        mutexUnlock(&g_log_mtx);
        threadWaitForExit(&g_writer);
        threadClose(&g_writer);
        g_writer_running = 0;
    }
    mutexLock(&g_log_mtx);
    g_log_init = 0;
    if (g_log_f && g_log_f != stderr) fclose(g_log_f);
    g_log_f = NULL;
    mutexUnlock(&g_log_mtx);
}

void engine_log_set_level(int level) {
    if (level < ENGINE_LOG_ERROR) level = ENGINE_LOG_ERROR;
    if (level > ENGINE_LOG_DEBUG) level = ENGINE_LOG_DEBUG;
    g_log_level = level;
}

void engine_log(int level, const char *fmt, ...) {
    if (!g_log_init || level > g_log_level || !g_log_f) return;

    const char *lvl = (level == ENGINE_LOG_ERROR) ? "ERR" :
                      (level == ENGINE_LOG_WARN)  ? "WRN" :
                      (level == ENGINE_LOG_INFO)  ? "INF" : "DBG";
    char line[LOG_LINE_MAX];
    char ts[32];

    mutexLock(&g_log_mtx);
    time_t now = wall_seconds();
    if (now != g_cached_second) {
        g_cached_second = (int)now;
        struct tm *tm = localtime(&now);
        strftime(g_cached_ts, sizeof(g_cached_ts), "%Y-%m-%d %H:%M:%S", tm);
    }
    memcpy(ts, g_cached_ts, sizeof(ts));
    mutexUnlock(&g_log_mtx);

    // Format outside the lock: the expensive part of a log call.
    int len = snprintf(line, sizeof(line), "[%s %s] ", ts, lvl);
    if (len < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + len, sizeof(line) - (size_t)len - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    len += m;
    if (len > (int)sizeof(line) - 2) len = (int)sizeof(line) - 2;   // truncated
    line[len++] = '\n';

    mutexLock(&g_log_mtx);
    if (!g_log_init || !g_log_f) {
        mutexUnlock(&g_log_mtx);
        return;
    }
    if (!g_writer_running) {
        // No writer thread: the old synchronous path.
        fwrite(line, 1, (size_t)len, g_log_f);
        if (level <= ENGINE_LOG_WARN) fflush(g_log_f);
    } else if (g_ring_len + (size_t)len > LOG_RING_SIZE) {
        g_dropped++;
    } else {
        ring_put(line, (size_t)len);
        // Errors should hit the disk promptly; a filling ring must drain
        // before it overflows. Everything else waits for the periodic flush.
        if (level <= ENGINE_LOG_WARN || g_ring_len > LOG_RING_SIZE / 2)
            condvarWakeAll(&g_log_cv);
    }
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
