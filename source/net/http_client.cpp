#include "http_client.h"
#include "../utils/log.h"

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#include <cstring>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <cstdio>

#if __has_include(<curl/curl.h>)
#define TSNX_HAVE_CURL 1
#include <curl/curl.h>
#else
#define TSNX_HAVE_CURL 0
#endif

namespace net {

HttpClient::HttpClient() = default;

HttpClient::~HttpClient() {
#if TSNX_HAVE_CURL
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
#endif
}

// =============================================================================
// URL-парсинг
// =============================================================================
// Non-blocking connect with timeout. Avoids blocking the caller for the OS
// default TCP timeout when a host is unreachable (common for dead image hosts).
static int connect_with_timeout(int sock, const struct sockaddr* addr,
                                socklen_t addrlen, int timeout_sec) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = connect(sock, addr, addrlen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLOUT;
    rc = poll(&pfd, 1, timeout_sec * 1000);
    if (rc <= 0) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    fcntl(sock, F_SETFL, flags);
    return 0;
}

bool HttpClient::parseUrl(const std::string& url, std::string& host, std::string& path, int& port) {
    host.clear();
    path = "/";
    port = 80;

    const std::string http_prefix = "http://";
    if (url.rfind(http_prefix, 0) != 0) {
        return false;
    }
    std::string rest = url.substr(http_prefix.size());
    std::string::size_type slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    } else {
        host = rest;
    }

    std::string::size_type colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.substr(colon + 1).c_str());
        host = host.substr(0, colon);
    }
    return !host.empty();
}

static bool isHttps(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

std::string HttpClient::buildRangeHeader(uint64_t offset, uint64_t length) const {
    std::ostringstream oss;
    oss << "Range: bytes=" << offset << "-";
    if (length > 0) {
        oss << (offset + length - 1);
    }
    return oss.str();
}

// =============================================================================
// curl callbacks
// =============================================================================
#if TSNX_HAVE_CURL

static CURLSH* g_curlShareHandle = nullptr;
static std::mutex g_shareMutexes[CURL_LOCK_DATA_LAST];

static void curlShareLock(CURL*, curl_lock_data data, curl_lock_access, void*) {
    if (data < CURL_LOCK_DATA_LAST) {
        g_shareMutexes[data].lock();
    }
}

static void curlShareUnlock(CURL*, curl_lock_data data, void*) {
    if (data < CURL_LOCK_DATA_LAST) {
        g_shareMutexes[data].unlock();
    }
}

static void initGlobalCurlShare() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        g_curlShareHandle = curl_share_init();
        if (g_curlShareHandle) {
            curl_share_setopt(g_curlShareHandle, CURLSHOPT_LOCKFUNC, curlShareLock);
            curl_share_setopt(g_curlShareHandle, CURLSHOPT_UNLOCKFUNC, curlShareUnlock);
            curl_share_setopt(g_curlShareHandle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(g_curlShareHandle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        }
    });
}

struct CurlXferContext {
    const std::atomic<bool>* cancel_flag = nullptr;
    ProgressCallback progress_cb = nullptr;
};

static int curlXferInfoCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    if (g_appExiting.load()) {
        return 1;
    }
    if (clientp) {
        auto* ctx = static_cast<CurlXferContext*>(clientp);
        if (ctx->cancel_flag && ctx->cancel_flag->load()) {
            return 1;
        }
        if (ctx->progress_cb) {
            ctx->progress_cb(static_cast<int64_t>(dltotal), static_cast<int64_t>(dlnow));
        }
    }
    return 0;
}

static CURL* getReusableCurl(void*& handle) {
    auto* curl = static_cast<CURL*>(handle);
    if (!curl) {
        curl = curl_easy_init();
        handle = curl;
    }
    if (curl) {
        initGlobalCurlShare();
        if (g_curlShareHandle) {
            curl_easy_setopt(curl, CURLOPT_SHARE, g_curlShareHandle);
        }
    }
    return curl;
}

static size_t curlWriteBody(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t curlWriteHeader(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct CurlStreamCtx {
    StreamCallback* cb;
    size_t total = 0;
};

static size_t curlWriteStream(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<CurlStreamCtx*>(userdata);
    size_t bytes = size * nmemb;
    if (ctx->cb) {
        size_t processed = (*ctx->cb)(ptr, bytes);
        ctx->total += processed;
    }
    return bytes;
}

struct FileWriteCtx {
    FILE* fp = nullptr;
    size_t written = 0;
};

static size_t curlWriteToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<FileWriteCtx*>(userdata);
    if (!ctx || !ctx->fp) return 0;
    size_t bytes = size * nmemb;
    size_t w = std::fwrite(ptr, 1, bytes, ctx->fp);
    ctx->written += w;
    return w;
}
#endif

// =============================================================================
// request() — основной метод
// =============================================================================
HttpResponse HttpClient::request(const std::string& method, const std::string& url,
                                  const std::string& body,
                                  const std::vector<std::string>& extra_headers) {
    HttpResponse resp;

    const bool use_curl = isHttps(url) || url.rfind("http://", 0) == 0;
    if (use_curl) {
#if TSNX_HAVE_CURL
        CURL* curl = getReusableCurl(curl_handle_);
        if (!curl) {
            resp.status_code = 0;
            resp.body = "curl init failed";
            return resp;
        }

        curl_easy_reset(curl);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        if (method == "POST") {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        for (const auto& h : extra_headers) {
            headers = curl_slist_append(headers, h.c_str());
        }

        CurlXferContext xfer_ctx{cancel_flag_, progress_cb_};

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlWriteHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::min(timeout_sec_, 10)));
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferInfoCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer_ctx);

        if (keep_alive_) {
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 10L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
        }

        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            resp.status_code = static_cast<int>(code);
        } else {
            resp.status_code = 0;
            resp.body = curl_easy_strerror(res);
        }

        curl_slist_free_all(headers);
        return resp;
#else
        resp.status_code = 0;
        resp.body = "HTTP client requires libcurl in this build";
        return resp;
#endif
    }

    // HTTP через сокеты
    std::string host, path;
    int port = 80;
    if (!parseUrl(url, host, path, port)) {
        resp.status_code = 0;
        resp.body = "Unsupported URL";
        return resp;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        resp.status_code = 0;
        resp.body = "DNS lookup failed";
        return resp;
    }

    int sock = -1;
    int last_errno = 0;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == -1) {
            last_errno = errno;
            continue;
        }
        if (connect_with_timeout(sock, rp->ai_addr, rp->ai_addrlen,
                                 timeout_sec_ > 0 ? timeout_sec_ : 30) == 0) {
            break;
        }
        last_errno = errno;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);

    if (sock == -1) {
        resp.status_code = 0;
        resp.body = "Connection failed errno=" + std::to_string(last_errno);
        util::logLine("HttpClient: connection failed errno=" + std::to_string(last_errno) + " url=" + url);
        return resp;
    }

    // Prevent indefinite blocking on send/recv when peer keeps socket open.
    timeval tv{};
    tv.tv_sec = timeout_sec_ > 0 ? timeout_sec_ : 30;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "User-Agent: TorrentShopNX/0.2\r\n";
    if (method == "POST") {
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    for (const auto& h : extra_headers) {
        req << h << "\r\n";
    }
    // For simple request/response mode over raw sockets, close is safer:
    // some servers keep HTTP/1.1 connections alive and recv() would block.
    req << "Connection: close\r\n\r\n";
    if (method == "POST") {
        req << body;
    }

    std::string req_str = req.str();
    size_t sent_total = 0;
    while (sent_total < req_str.size()) {
        int sent = send(sock, req_str.data() + sent_total, req_str.size() - sent_total, 0);
        if (sent <= 0) {
            close(sock);
            resp.status_code = 0;
            resp.body = "Send failed";
            return resp;
        }
        sent_total += static_cast<size_t>(sent);
    }

    std::string response;
    char buffer[4096];
    int n = 0;
    while ((n = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[n] = 0;
        response.append(buffer);
    }
    close(sock);

    // Разделяем заголовки и тело
    std::string::size_type header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        resp.status_code = 0;
        resp.body = response;
        return resp;
    }

    resp.headers = response.substr(0, header_end);
    resp.body = response.substr(header_end + 4);

    // Парсим код статуса
    std::istringstream status_line(resp.headers);
    std::string http_ver;
    status_line >> http_ver >> resp.status_code;

    return resp;
}

// =============================================================================
// Публичные методы
// =============================================================================
HttpResponse HttpClient::httpGet(const std::string& url) {
    return request("GET", url, "", {});
}

HttpResponse HttpClient::httpGet(const std::string& url, const std::vector<std::string>& extra_headers) {
    return request("GET", url, "", extra_headers);
}

HttpResponse HttpClient::httpPost(const std::string& url, const std::string& json_body,
                                   const std::vector<std::string>& extra_headers) {
    return request("POST", url, json_body, extra_headers);
}

// =============================================================================
// httpGetStream — потоковый GET с Range и callback
// =============================================================================
int HttpClient::httpGetStream(const std::string& url, uint64_t offset, uint64_t length,
                               StreamCallback cb, const std::atomic<bool>* cancel_flag) {
#if TSNX_HAVE_CURL
    CURL* curl = getReusableCurl(curl_handle_);
    if (!curl) return 0;

    curl_easy_reset(curl);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: TorrentShopNX/0.2");

    // Range-заголовок
    if (offset > 0 || length > 0) {
        std::string range = buildRangeHeader(offset, length);
        headers = curl_slist_append(headers, range.c_str());
    }

    if (keep_alive_) {
        headers = curl_slist_append(headers, "Connection: keep-alive");
    }
    headers = curl_slist_append(headers, "Accept-Encoding: identity");

    CurlStreamCtx ctx;
    ctx.cb = &cb;
    ctx.total = 0;

    const std::atomic<bool>* effective_flag = cancel_flag ? cancel_flag : cancel_flag_;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteStream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::min(timeout_sec_, 10)));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferInfoCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, effective_flag);

    if (keep_alive_) {
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 10L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);

    return static_cast<int>(http_code);
#else
    // Socket fallback c Range-заголовком
    std::vector<std::string> hdrs;
    if (offset > 0 || length > 0) {
        hdrs.push_back(buildRangeHeader(offset, length));
    }

    auto resp = httpGet(url, hdrs);
    if (resp.status_code >= 200 && resp.status_code < 300 && !resp.body.empty() && cb) {
        cb(resp.body.data(), resp.body.size());
    }
    return resp.status_code;
#endif
}

// =============================================================================
// downloadToFile — потоковая загрузка прямо в файл на диске
// =============================================================================
bool HttpClient::downloadToFile(const std::string& url, const std::string& dest_path,
                                const std::atomic<bool>* cancel_flag, int timeout_sec) {
    std::filesystem::path p(dest_path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    FILE* fp = std::fopen(dest_path.c_str(), "wb");
    if (!fp) {
        util::logLine("HttpClient: failed to open dest file for writing: " + dest_path);
        return false;
    }

#if TSNX_HAVE_CURL
    CURL* curl = getReusableCurl(curl_handle_);
    if (!curl) {
        std::fclose(fp);
        std::remove(dest_path.c_str());
        return false;
    }

    curl_easy_reset(curl);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Nintendo Switch; TorrentShopNX/2.6)");

    FileWriteCtx file_ctx{fp, 0};
    CurlXferContext xfer_ctx{cancel_flag ? cancel_flag : cancel_flag_, progress_cb_};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file_ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec > 0 ? timeout_sec : 180));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // 1 KB/s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);    // 15 seconds stall timeout
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");    // automatic gzip/deflate decompression!
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferInfoCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer_ctx);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    std::fflush(fp);
    std::fclose(fp);

    if (res == CURLE_OK && http_code == 200 && file_ctx.written > 0) {
        util::logLine("HttpClient: downloadToFile succeeded for " + url + " bytes=" + std::to_string(file_ctx.written));
        return true;
    }

    util::logLine("HttpClient: downloadToFile failed for " + url + " res=" + std::to_string(res) +
                  " (" + curl_easy_strerror(res) + ") http_code=" + std::to_string(http_code) +
                  " written=" + std::to_string(file_ctx.written));
    std::remove(dest_path.c_str());
    return false;
#else
    std::fclose(fp);
    auto resp = httpGet(url);
    if (resp.status_code == 200 && !resp.body.empty()) {
        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out.write(resp.body.data(), resp.body.size());
            return true;
        }
    }
    std::remove(dest_path.c_str());
    return false;
#endif
}

} // namespace net
