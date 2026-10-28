// Leaves a trace in log.txt for the ways the app can quit without an error
// screen. On the console newlib's abort() is raise() + _exit(1): the app just
// returns to the menu, with no Atmosphere crash report and nothing in the log.
// assert() in the prebuilt libraries (curl, libarchive, zstd, mesa...) and the
// sanity checks of the DHT code end up there.
//
// These definitions replace the libc ones at link time (an archive member is
// only pulled in for symbols nothing else defines). The normal shutdown leaves
// through _exit(), so an exit() call is always unexpected.

#ifdef __SWITCH__

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "log.h"
#include "ui_watchdog.h"

namespace {

std::atomic<bool> g_dying{false};

// First caller logs, flushes and exits; a nested call (abort from inside the
// logger, say) exits at once instead of deadlocking.
[[noreturn]] void traceAndExit(const std::string& what, const char* tag, int code) {
    if (!g_dying.exchange(true)) {
        util::logLine("FATAL: " + what);
        uiwd::logCurrentStack(tag);
        util::logFlush();
    }
    _exit(code);
}

void onUnexpectedExit() {
    if (g_dying.exchange(true)) return;
    util::logLine("FATAL: exit() called (unexpected app exit)");
    uiwd::logCurrentStack("exit");
    util::logFlush();
}

struct RegisterExitHook {
    RegisterExitHook() { std::atexit(onUnexpectedExit); }
} g_registerExitHook;

}  // namespace

extern "C" {

[[noreturn]] void abort(void) {
    traceAndExit("abort() called", "abort", 1);
}

[[noreturn]] void __assert_func(const char* file, int line, const char* func, const char* expr) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "assertion \"%s\" failed: file \"%s\", line %d%s%s", expr ? expr : "?",
                  file ? file : "?", line, func ? ", function: " : "", func ? func : "");
    traceAndExit(buf, "assert", 1);
}

// Same libc member as __assert_func: defined too so it is never pulled in.
[[noreturn]] void __assert(const char* file, int line, const char* expr) {
    __assert_func(file, line, nullptr, expr);
}

}  // extern "C"

#endif  // __SWITCH__
