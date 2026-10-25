#pragma once

#include <functional>

namespace util {

// Runs fn on a thread of its own and returns immediately.
//
// Use this instead of std::thread(...).detach(): libnx has no pthread_detach,
// so on the console detach() always throws, and the still-joinable
// std::thread then calls std::terminate (the app just closes, no crash
// report). Threads started here are kept and joined once they have finished;
// one that is still running when the app exits is simply left behind.
//
// An exception escaping fn is logged instead of terminating the app.
// Returns false (and logs) when the thread cannot be created.
bool runInBackground(std::function<void()> fn, const char* name = "bg");

} // namespace util
