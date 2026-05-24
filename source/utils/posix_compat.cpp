#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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

    int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int err = errno;
        ::close(listener);
        errno = err;
        return -1;
    }

    if (::listen(listener, 1) != 0) {
        int err = errno;
        ::close(listener);
        errno = err;
        return -1;
    }

    socklen_t addr_len = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        int err = errno;
        ::close(listener);
        errno = err;
        return -1;
    }

    int writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (writer < 0) {
        int err = errno;
        ::close(listener);
        errno = err;
        return -1;
    }

    if (::connect(writer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int err = errno;
        ::close(writer);
        ::close(listener);
        errno = err;
        return -1;
    }

    int reader = ::accept(listener, nullptr, nullptr);
    if (reader < 0) {
        int err = errno;
        ::close(writer);
        ::close(listener);
        errno = err;
        return -1;
    }

    ::close(listener);
    pipefd[0] = reader;
    pipefd[1] = writer;
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
