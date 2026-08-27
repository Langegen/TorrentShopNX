#pragma once

#ifdef __SWITCH__
#include <sys/_iovec.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#else
#include_next <sys/uio.h>
#endif
