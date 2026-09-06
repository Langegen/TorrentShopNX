#ifdef __SWITCH__

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "log.h"
#include <string>
#include <errno.h>
#include <cstring>
#include <mutex>
#include <signal.h>

static std::mutex g_file_io_mutex;

extern "C" ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    std::lock_guard<std::mutex> lock(g_file_io_mutex);
    off_t current = ::lseek(fd, 0, SEEK_CUR);
    if (current == (off_t)-1) return -1;
    if (::lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    ssize_t n = ::read(fd, buf, count);
    ::lseek(fd, current, SEEK_SET);
    return n;
}

extern "C" ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    std::lock_guard<std::mutex> lock(g_file_io_mutex);
    off_t current = ::lseek(fd, 0, SEEK_CUR);
    if (current == (off_t)-1) return -1;
    if (::lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    ssize_t n = ::write(fd, buf, count);
    ::lseek(fd, current, SEEK_SET);
    return n;
}

extern "C" ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return 0;

    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        ssize_t n = ::read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (total > 0) ? total : -1;
        total += n;
        if (static_cast<size_t>(n) < iov[i].iov_len) break;
    }
    return total;
}

extern "C" ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return 0;

    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        ssize_t n = ::write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (total > 0) ? total : -1;
        total += n;
        if (static_cast<size_t>(n) < iov[i].iov_len) break;
    }
    return total;
}

extern "C" unsigned int if_nametoindex(const char* ifname) {
    if (!ifname || !*ifname) return 0;
    if (std::strcmp(ifname, "wlan0") == 0) return 1;
    if (std::strcmp(ifname, "eth0") == 0) return 1;
    return 0;
}

extern "C" char* if_indextoname(unsigned int ifindex, char* ifname) {
    if (!ifname || ifindex == 0) return nullptr;
    std::strcpy(ifname, "wlan0");
    return ifname;
}

extern "C" int pipe(int pipefd[2]) {
    if (!pipefd) {
        errno = EINVAL;
        return -1;
    }

    pipefd[0] = -1;
    pipefd[1] = -1;

    util::logLine("posix_compat: pipe() called (UDP socketpair)");

    int s1 = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s1 < 0) {
        int err = errno;
        util::logLine("posix_compat: UDP socket 1 creation failed: " + std::to_string(err));
        errno = err;
        return -1;
    }

    int buf_size = 4096;
    ::setsockopt(s1, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    ::setsockopt(s1, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));

    // Ensure non-blocking mode on s1 via fcntl and ioctl
    int flags1 = ::fcntl(s1, F_GETFL, 0);
    if (flags1 != -1) {
        ::fcntl(s1, F_SETFL, flags1 | O_NONBLOCK);
    }
    int nonblock_opt = 1;
    ::ioctl(s1, FIONBIO, &nonblock_opt);

    timeval tv{2, 0}; // 2-second fallback timeout
    ::setsockopt(s1, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(s1, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in addr1{};
    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr1.sin_port = 0;

    if (::bind(s1, reinterpret_cast<sockaddr*>(&addr1), sizeof(addr1)) != 0) {
        int err = errno;
        util::logLine("posix_compat: UDP bind 1 failed: " + std::to_string(err));
        ::close(s1);
        errno = err;
        return -1;
    }

    socklen_t addr1_len = sizeof(addr1);
    if (::getsockname(s1, reinterpret_cast<sockaddr*>(&addr1), &addr1_len) != 0) {
        int err = errno;
        util::logLine("posix_compat: UDP getsockname 1 failed: " + std::to_string(err));
        ::close(s1);
        errno = err;
        return -1;
    }

    int s2 = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s2 < 0) {
        int err = errno;
        util::logLine("posix_compat: UDP socket 2 creation failed: " + std::to_string(err));
        ::close(s1);
        errno = err;
        return -1;
    }

    ::setsockopt(s2, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    ::setsockopt(s2, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));

    // Ensure non-blocking mode on s2 via fcntl and ioctl
    int flags2 = ::fcntl(s2, F_GETFL, 0);
    if (flags2 != -1) {
        ::fcntl(s2, F_SETFL, flags2 | O_NONBLOCK);
    }
    ::ioctl(s2, FIONBIO, &nonblock_opt);
    ::setsockopt(s2, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(s2, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in addr2{};
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr2.sin_port = 0;

    if (::bind(s2, reinterpret_cast<sockaddr*>(&addr2), sizeof(addr2)) != 0) {
        int err = errno;
        util::logLine("posix_compat: UDP bind 2 failed: " + std::to_string(err));
        ::close(s2);
        ::close(s1);
        errno = err;
        return -1;
    }

    socklen_t addr2_len = sizeof(addr2);
    if (::getsockname(s2, reinterpret_cast<sockaddr*>(&addr2), &addr2_len) != 0) {
        int err = errno;
        util::logLine("posix_compat: UDP getsockname 2 failed: " + std::to_string(err));
        ::close(s2);
        ::close(s1);
        errno = err;
        return -1;
    }

    // Connect s1 to s2
    if (::connect(s1, reinterpret_cast<sockaddr*>(&addr2), sizeof(addr2)) != 0) {
        int err = errno;
        util::logLine("posix_compat: UDP connect 1->2 failed: " + std::to_string(err));
        ::close(s2);
        ::close(s1);
        errno = err;
        return -1;
    }

    // Connect s2 to s1
    if (::connect(s2, reinterpret_cast<sockaddr*>(&addr1), sizeof(addr1)) != 0) {
        int err = errno;
        util::logLine("posix_compat: UDP connect 2->1 failed: " + std::to_string(err));
        ::close(s2);
        ::close(s1);
        errno = err;
        return -1;
    }

    util::logLine("posix_compat: UDP pipe() succeeded, port1=" + std::to_string(ntohs(addr1.sin_port)) + " port2=" + std::to_string(ntohs(addr2.sin_port)));

    pipefd[0] = s1;
    pipefd[1] = s2;
    return 0;
}

extern "C" int pause(void) {
    ::usleep(100000);
    errno = EINTR;
    return -1;
}

extern "C" int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    (void)how;
    (void)set;
    (void)oldset;
    return 0;
}

extern "C" mode_t umask(mode_t mask) {
    static mode_t s_mask = 022;
    mode_t old = s_mask;
    s_mask = mask;
    return old;
}

#endif // __SWITCH__

