#include "ui_watchdog.h"
#include "log.h"

#ifdef __SWITCH__
#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include "../engine/thread_cfg.h"

extern "C" {
extern char __code_start;     // switch.ld: start of .text (link address 0)
extern char __rodata_start;   // switch.ld: end of executable code
}

namespace uiwd {
namespace {

constexpr u64 kStallNs      = 500ULL * 1000 * 1000;   // frame gap that counts as a stall
constexpr u64 kSampleNs     = 500ULL * 1000 * 1000;   // between samples of one stall
constexpr int kMaxSamples   = 8;                      // per stall
constexpr u64 kPollNs       = 100ULL * 1000 * 1000;
constexpr int kStackWords   = 1024;                   // 8 KB of stack scanned
constexpr int kMaxFrames    = 14;

std::atomic<u64> g_last_beat_ns{0};
std::atomic<bool> g_running{false};
Thread g_thread;
Handle g_ui_handle = INVALID_HANDLE;
u64 g_code_lo = 0, g_code_hi = 0;

u64 nowNs() { return armTicksToNs(armGetSystemTick()); }

bool inCode(u64 a) { return a >= g_code_lo && a < g_code_hi; }

struct Sample {
    bool ok = false;
    Result rc = 0;
    u64 pc = 0, lr = 0;
    int nframes = 0;
    u64 frames[kMaxFrames];
};

// Pause the UI thread, copy what we need, resume. Nothing in between may take
// a lock or allocate: the paused thread might hold the lock.
Sample sampleUi() {
    static u64 stack_copy[kStackWords];
    Sample s;
    Result rc = svcSetThreadActivity(g_ui_handle, ThreadActivity_Paused);
    if (R_FAILED(rc)) { s.rc = rc; return s; }

    ThreadContext ctx;
    // The pause takes effect when the thread leaves the core; retry briefly.
    for (int i = 0; i < 50; i++) {
        rc = svcGetThreadContext3(&ctx, g_ui_handle);
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(100000);   // 0.1 ms
    }
    int nwords = 0;
    if (R_SUCCEEDED(rc)) {
        MemoryInfo mi;
        u32 pi = 0;
        u64 sp = ctx.sp;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, sp)) && (mi.perm & Perm_R)) {
            u64 end = mi.addr + mi.size;
            u64 avail = (end > sp) ? (end - sp) / 8 : 0;
            nwords = (int)(avail < (u64)kStackWords ? avail : (u64)kStackWords);
            std::memcpy(stack_copy, reinterpret_cast<const void*>(sp), (size_t)nwords * 8);
        }
    }
    svcSetThreadActivity(g_ui_handle, ThreadActivity_Runnable);

    if (R_FAILED(rc)) { s.rc = rc; return s; }
    s.ok = true;
    s.pc = ctx.pc.x;
    s.lr = ctx.lr;
    // Return addresses on the stack (the build omits frame pointers, so walk
    // the raw words): every value pointing into our code is a candidate.
    for (int i = 0; i < nwords && s.nframes < kMaxFrames; i++) {
        u64 v = stack_copy[i];
        if (inCode(v) && (v & 3) == 0) s.frames[s.nframes++] = v;
    }
    return s;
}

void logSample(u64 gap_ms, int idx, const Sample& s) {
    char buf[512];
    if (!s.ok) {
        std::snprintf(buf, sizeof(buf), "[uistall] gap=%llums sample=%d context unavailable rc=0x%x",
                      (unsigned long long)gap_ms, idx, (unsigned)s.rc);
        util::logLine(buf);
        return;
    }
    auto off = [](u64 a) -> unsigned long long {
        return inCode(a) ? (unsigned long long)(a - g_code_lo) : 0ULL;
    };
    int len = std::snprintf(buf, sizeof(buf), "[uistall] gap=%llums sample=%d pc=0x%llx lr=0x%llx stack=",
                            (unsigned long long)gap_ms, idx, off(s.pc), off(s.lr));
    for (int i = 0; i < s.nframes && len > 0 && len < (int)sizeof(buf) - 20; i++)
        len += std::snprintf(buf + len, sizeof(buf) - len, "%s0x%llx", i ? "," : "", off(s.frames[i]));
    util::logLine(buf);
}

void threadMain(void*) {
    u64 stall_beat = 0;      // beat value the current stall started from
    int samples = 0;
    u64 next_sample = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        svcSleepThread(kPollNs);
        u64 beat = g_last_beat_ns.load(std::memory_order_relaxed);
        if (beat == 0) continue;
        u64 now = nowNs();
        u64 gap = now > beat ? now - beat : 0;
        if (gap < kStallNs || gap > 60ULL * 1000 * 1000 * 1000) {
            if (stall_beat != 0 && samples > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "[uistall] end, %d samples", samples);
                util::logLine(buf);
            }
            stall_beat = 0;
            samples = 0;
            continue;
        }
        // Home menu / sleep: the UI legitimately stops drawing.
        if (appletGetFocusState() != AppletFocusState_InFocus) continue;
        if (stall_beat != beat) {   // a new stall
            stall_beat = beat;
            samples = 0;
            next_sample = now;
        }
        if (samples < kMaxSamples && now >= next_sample) {
            Sample s = sampleUi();
            logSample(gap / 1000000ULL, samples, s);
            samples++;
            next_sample = now + kSampleNs;
        }
    }
}

} // namespace

void start() {
    if (g_running.load()) return;
    g_ui_handle = envGetMainThreadHandle();
    g_code_lo = reinterpret_cast<u64>(&__code_start);
    g_code_hi = reinterpret_cast<u64>(&__rodata_start);
    g_running.store(true);
    if (R_FAILED(threadCreate(&g_thread, threadMain, nullptr, nullptr, 0x8000,
                              TSNX_THREAD_ARGS(TSNX_CORE_WORK, 0x2B)))) {
        g_running.store(false);
        util::logLine("uiwd: thread create failed");
        return;
    }
    threadStart(&g_thread);
}

void beat() {
    g_last_beat_ns.store(nowNs(), std::memory_order_relaxed);
}

void stop() {
    if (!g_running.exchange(false)) return;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

} // namespace uiwd

#else

namespace uiwd {
void start() {}
void beat() {}
void stop() {}
} // namespace uiwd

#endif
