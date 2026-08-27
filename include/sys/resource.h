#pragma once

#ifdef __SWITCH__
#include <sys/types.h>

typedef unsigned long rlim_t;

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIMIT_NOFILE 0
#define RLIMIT_AS     1
#define RLIM_INFINITY ((rlim_t)-1)

#ifdef __cplusplus
extern "C" {
#endif

inline int getrlimit(int resource, struct rlimit *rl) {
    if (!rl) return -1;
    if (resource == RLIMIT_NOFILE) {
        rl->rlim_cur = 1024;
        rl->rlim_max = 1024;
    } else {
        rl->rlim_cur = RLIM_INFINITY;
        rl->rlim_max = RLIM_INFINITY;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#else
#include_next <sys/resource.h>
#endif
