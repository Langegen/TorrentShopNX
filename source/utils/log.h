#pragma once

#include <string>
#include <atomic>

namespace util {

void logInit();
void logLine(const std::string& line);
void logClose();

} // namespace util

extern std::atomic<bool> g_appExiting;

