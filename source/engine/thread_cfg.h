#ifndef TSNX_THREAD_CFG_H
#define TSNX_THREAD_CFG_H

// Where every long-lived thread runs, in one place.
//
// The Switch gives an application cores 0..2 (core 3 belongs to the system).
// Everything used to be created with core -2 (the process default, core 0) --
// the same core as the borealis UI thread -- and the netloop ran at a HIGHER
// priority than the UI, so a busy download preempted every frame. The layout:
//
//   core 0  UI (borealis main thread) only
//   core 1  heavy app work: installer, collector, piece verify/writer, image
//           download + decode, loggers
//   core 2  network: netloop, discovery/DHT/tracker/listener/UPnP, metadata
//           fetch workers, the download progress thread
//
// Lower number = higher priority (libnx). The UI thread runs at 0x2C; on
// cores 1 and 2 nothing competes with it, so these values only order the
// threads that share a core.

#ifdef __SWITCH__
#include <switch.h>
#endif

#define TSNX_CORE_UI    0
#define TSNX_CORE_WORK  1
#define TSNX_CORE_NET   2

#define TSNX_PRIO_NETLOOP    0x2B   // core 2: the hot loop, first among net threads
#define TSNX_PRIO_NET_BG     0x2E   // core 2: discovery, DHT, announce, listener, UPnP, meta workers
#define TSNX_PRIO_PROGRESS   0x2D   // core 2: DownloadManager progress thread
#define TSNX_PRIO_COLLECTOR  0x2C   // core 1: feeds the installer ring buffer (mostly waits)
#define TSNX_PRIO_WRITER     0x2D   // core 1: SHA-1 verify + cache write
#define TSNX_PRIO_IMAGE      0x2D   // core 1: cover/screenshot fetch + decode
#define TSNX_PRIO_LOGGER     0x2F   // core 1: async log writers (above the installer's 0x30)

// A thread that could not get its own core lands next to the UI (core 0):
// there it must run below the UI (0x2C), or it preempts rendering again.
#define TSNX_PRIO_BELOW_UI   0x2D

#ifdef __cplusplus
extern "C" {
#endif

// The core to pass to threadCreate(): `want` if the process may run there,
// otherwise -2 (process default). Applet mode and odd loaders can hand out a
// narrower core mask, so this is checked once against InfoType_CoreMask.
static inline int tsnx_thread_core(int want) {
#ifdef __SWITCH__
    static int s_mask = -1;   // benign race: every thread computes the same value
    if (s_mask < 0) {
        u64 m = 0;
        s_mask = R_SUCCEEDED(svcGetInfo(&m, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))
                     ? (int)(m & 0xF) : 0;
    }
    if (want >= 0 && want < 4 && (s_mask & (1 << want))) return want;
    return -2;
#else
    (void)want;
    return -2;
#endif
}

// `prio` on a non-UI core; never above TSNX_PRIO_BELOW_UI on the UI core
// (or on the process default core, which is the UI core).
static inline int tsnx_thread_prio(int core, int prio) {
    if (core < 0 || core == TSNX_CORE_UI)
        return prio > TSNX_PRIO_BELOW_UI ? prio : TSNX_PRIO_BELOW_UI;
    return prio;
}

// The last two threadCreate() arguments (priority, core) for a thread that
// wants `core` at `prio`, with the fallbacks above applied.
#define TSNX_THREAD_ARGS(core, prio)     tsnx_thread_prio(tsnx_thread_core(core), (prio)), tsnx_thread_core(core)

// Move the calling thread (e.g. a std::thread, which libnx creates on the
// default core) to `core` at `prio`. No-op off the Switch.
static inline void tsnx_thread_pin_self(int core, int prio) {
#ifdef __SWITCH__
    int c = tsnx_thread_core(core);
    if (c >= 0) svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c);
    svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)tsnx_thread_prio(c, prio));
#else
    (void)core;
    (void)prio;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
