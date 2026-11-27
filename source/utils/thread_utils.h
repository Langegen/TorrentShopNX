#pragma once

#include <functional>

namespace util {

// Runs fn on a thread of its own and returns immediately.
//
// Use this instead of std::thread(...).detach(): libnx has no pthread_detach,
// so on the console detach() always throws, and the still-joinable
// std::thread then calls std::terminate (the app just closes, no crash
// report). Threads started here are kept and joined once they have finished, or
// waited on at shutdown via joinBackgroundThreads().
//
// An exception escaping fn is logged instead of terminating the app.
// Returns false (and logs) when the thread cannot be created or shutdown has begun.
bool runInBackground(std::function<void()> fn, const char* name = "bg");

// Stops accepting new background tasks and waits up to timeout_ms for all
// running tasks to finish and join. Returns true iff every thread was joined.
bool joinBackgroundThreads(int timeout_ms = 5000);

} // namespace util
