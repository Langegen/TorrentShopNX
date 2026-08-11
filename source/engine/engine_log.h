#ifndef ENGINE_LOG_H
#define ENGINE_LOG_H

#include <stddef.h>

enum {
    ENGINE_LOG_ERROR = 0,
    ENGINE_LOG_WARN  = 1,
    ENGINE_LOG_INFO  = 2,
    ENGINE_LOG_DEBUG = 3
};

// Open the diagnostic log.  path == NULL (or a path that cannot be opened)
// falls back to stderr so PC tests still see output.
void engine_log_init(const char *path);
void engine_log_close(void);

// Set minimum level that actually gets written (default ENGINE_LOG_INFO).
void engine_log_set_level(int level);

void engine_log(int level, const char *fmt, ...);

#endif
