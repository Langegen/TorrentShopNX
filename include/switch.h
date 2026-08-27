#pragma once

#ifdef __SWITCH__
// On Nintendo Switch, use official libnx header
#include_next <switch.h>
#else

// Compatibility shim for non-Switch (PC / Desktop) builds
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void consoleUpdate(void* p) { (void)p; }

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef u32 Result;

#define R_SUCCEEDED(res)   ((res) == 0)
#define R_FAILED(res)      ((res) != 0)

//-----------------------------------------------------------------------------
// Mutex
//-----------------------------------------------------------------------------
typedef struct { pthread_mutex_t m; int init; } Mutex;

static inline void mutexInit(Mutex *mtx) {
    if (!mtx) return;
    pthread_mutex_init(&mtx->m, NULL);
    mtx->init = 1;
}

static inline void mutexLock(Mutex *mtx) {
    if (!mtx) return;
    if (!mtx->init) mutexInit(mtx);
    pthread_mutex_lock(&mtx->m);
}

static inline void mutexUnlock(Mutex *mtx) {
    if (!mtx) return;
    pthread_mutex_unlock(&mtx->m);
}

//-----------------------------------------------------------------------------
// Condition variable
//-----------------------------------------------------------------------------
typedef struct { pthread_cond_t c; int init; } CondVar;

static inline void condvarInit(CondVar *cv) {
    if (!cv) return;
    pthread_cond_init(&cv->c, NULL);
    cv->init = 1;
}

static inline void condvarWakeAll(CondVar *cv) {
    if (!cv) return;
    if (!cv->init) condvarInit(cv);
    pthread_cond_broadcast(&cv->c);
}

static inline void condvarWakeOne(CondVar *cv) {
    if (!cv) return;
    if (!cv->init) condvarInit(cv);
    pthread_cond_signal(&cv->c);
}

static inline Result condvarWaitTimeout(CondVar *cv, Mutex *mtx, u64 timeout_ns) {
    if (!cv || !mtx) return 0;
    if (!cv->init) condvarInit(cv);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(timeout_ns / 1000000000ULL);
    ts.tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return (Result)pthread_cond_timedwait(&cv->c, &mtx->m, &ts);
}

//-----------------------------------------------------------------------------
// Thread
//-----------------------------------------------------------------------------
typedef void (*ThreadFunc)(void *);
typedef struct {
    pthread_t pt;
    ThreadFunc entry;
    void *arg;
    u32 handle;
} Thread;

static inline Result threadCreate(Thread *t, ThreadFunc entry, void *arg,
                                  void *stack, size_t stack_sz, int prio, int cpu) {
    (void)stack; (void)stack_sz; (void)prio; (void)cpu;
    if (!t) return 1;
    t->entry = entry;
    t->arg = arg;
    t->handle = 1;
    return 0;
}

static inline void *pc_thread_trampoline(void *p) {
    Thread *t = (Thread *)p;
    if (t && t->entry) t->entry(t->arg);
    return NULL;
}

static inline Result threadStart(Thread *t) {
    if (!t) return 1;
    return (Result)pthread_create(&t->pt, NULL, pc_thread_trampoline, t);
}

static inline Result threadWaitForExit(Thread *t) {
    if (!t) return 1;
    pthread_join(t->pt, NULL);
    return 0;
}

static inline Result threadClose(Thread *t) {
    if (!t) return 1;
    t->handle = 0;
    return 0;
}

//-----------------------------------------------------------------------------
// Timing
//-----------------------------------------------------------------------------
static inline u64 armGetSystemTick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static inline u64 armGetSystemTickFreq(void) { return 1000000000ULL; }
static inline u64 armNsToTicks(u64 ns) { return ns; }
static inline u64 armTicksToNs(u64 t) { return t; }

static inline void svcSleepThread(u64 ns) {
    struct timespec ts = { (time_t)(ns / 1000000000ULL),
                           (long)(ns % 1000000000ULL) };
    nanosleep(&ts, NULL);
}

static inline u32 svcGetCurrentProcessorNumber(void) { return 0; }

//-----------------------------------------------------------------------------
// Random
//-----------------------------------------------------------------------------
static inline void randomGet(void *buf, size_t len) {
    static int seeded = 0;
    if (!seeded) { srand((unsigned)(armGetSystemTick() ^ (u64)(uintptr_t)&seeded)); seeded = 1; }
    u8 *b = (u8 *)buf;
    for (size_t i = 0; i < len; i++) b[i] = (u8)(rand() & 0xFF);
}

#ifdef __cplusplus
}
#endif

#endif // !__SWITCH__
