#pragma once

#include <string>
#include <atomic>

namespace util {

// logInit() starts an asynchronous writer: logLine() only appends to a memory
// buffer, and lines reach log.txt within ~0.5 s. logClose() writes out
// everything pending; logFlush() waits (<= 1 s) until it is on disk.
void logInit();
void logLine(const std::string& line);
void logFlush();
void logClose();

} // namespace util

extern std::atomic<bool> g_appExiting;

