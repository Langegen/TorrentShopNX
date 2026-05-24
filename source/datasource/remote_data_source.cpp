#include "remote_data_source.h"
#include "../utils/log.h"

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

#if __has_include(<curl/curl.h>)
#define TSNX_HAVE_CURL 1
#include <curl/curl.h>
#else
#define TSNX_HAVE_CURL 0
#endif

namespace {

constexpr size_t kLocalProxyRangeSlice = 1024 * 1024;

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseU64(const std::string& value, uint64_t& out) {
    const std::string trimmed = trimCopy(value);
    if (trimmed.empty()) return false;

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str()) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end != '\0') return false;

    out = static_cast<uint64_t>(parsed);
    return true;
}

struct RangeResponseInfo {
    bool has_content_range = false;
    bool has_total_size = false;
    bool encoded = false;
    uint64_t range_start = 0;
    uint64_t range_end = 0;
    uint64_t total_size = 0;
};

void parseRangeHeaderLine(const std::string& raw_line, RangeResponseInfo& info) {
    const std::string line = trimCopy(raw_line);
    if (line.empty()) return;

    const size_t colon = line.find(':');
    if (colon == std::string::npos) return;

    const std::string name = toLowerCopy(trimCopy(line.substr(0, colon)));
    const std::string value = trimCopy(line.substr(colon + 1));

    if (name == "content-encoding") {
        const std::string lower_value = toLowerCopy(value);
        if (!lower_value.empty() && lower_value != "identity") {
            info.encoded = true;
        }
        return;
    }

    if (name != "content-range") return;

    const std::string lower_value = toLowerCopy(value);
    const size_t bytes_pos = lower_value.find("bytes");
    if (bytes_pos == std::string::npos) return;

    std::string range_part = trimCopy(value.substr(bytes_pos + 5));
    const size_t dash = range_part.find('-');
    const size_t slash = range_part.find('/');
    if (dash == std::string::npos || slash == std::string::npos || dash >= slash) return;

    uint64_t start = 0;
    uint64_t end = 0;
    if (!parseU64(range_part.substr(0, dash), start) ||
        !parseU64(range_part.substr(dash + 1, slash - dash - 1), end)) {
        return;
    }

    info.has_content_range = true;
    info.range_start = start;
    info.range_end = end;

    const std::string total_str = trimCopy(range_part.substr(slash + 1));
    if (total_str != "*") {
        uint64_t total = 0;
        if (parseU64(total_str, total)) {
            info.has_total_size = true;
            info.total_size = total;
        }
    }
}

size_t expectedRangeSize(uint64_t known_file_size, uint64_t offset, size_t requested) {
    if (requested == 0) return 0;
    if (known_file_size == 0) return requested;
    if (offset >= known_file_size) return 0;
    return static_cast<size_t>(std::min<uint64_t>(requested, known_file_size - offset));
}

bool validateRangeResponse(const RangeResponseInfo& info,
                           long http_code,
                           uint64_t offset,
                           size_t requested,
                           size_t body_size,
                           bool body_overflowed,
                           uint64_t known_file_size,
                           std::string& reason) {
    const uint64_t effective_file_size =
        known_file_size > 0 ? known_file_size : (info.has_total_size ? info.total_size : 0);
    const size_t expected = expectedRangeSize(effective_file_size, offset, requested);
    if (expected == 0) {
        if (body_size != 0 || body_overflowed) {
            reason = "received data past EOF";
            return false;
        }
        return true;
    }

    if (http_code != 206) {
        reason = "expected HTTP 206, got " + std::to_string(http_code);
        return false;
    }
    if (info.encoded) {
        reason = "unexpected Content-Encoding in binary range response";
        return false;
    }
    if (!info.has_content_range) {
        reason = "missing Content-Range";
        return false;
    }
    if (body_overflowed) {
        reason = "response body exceeded target buffer";
        return false;
    }

    const uint64_t expected_end = offset + expected - 1;
    if (info.range_start != offset || info.range_end != expected_end) {
        reason = "Content-Range mismatch: got " + std::to_string(info.range_start) +
                 "-" + std::to_string(info.range_end) +
                 ", expected " + std::to_string(offset) +
                 "-" + std::to_string(expected_end);
        return false;
    }

    if (body_size != expected) {
        reason = "short body: got " + std::to_string(body_size) +
                 ", expected " + std::to_string(expected);
        return false;
    }

    if (known_file_size > 0 && info.has_total_size && info.total_size != known_file_size) {
        reason = "reported total size mismatch: got " + std::to_string(info.total_size) +
                 ", expected " + std::to_string(known_file_size);
        return false;
    }

    return true;
}

#if TSNX_HAVE_CURL
static size_t curlParseRangeHeader(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* info = static_cast<RangeResponseInfo*>(userdata);
    parseRangeHeaderLine(std::string(static_cast<const char*>(ptr), size * nmemb), *info);
    return size * nmemb;
}
#endif

} // namespace

namespace datasource {

#if TSNX_HAVE_CURL
static CURL* getReusableCurl(void*& handle) {
    auto* curl = static_cast<CURL*>(handle);
    if (!curl) {
        curl = curl_easy_init();
        handle = curl;
    }
    return curl;
}

struct CurlReadContext {
    uint8_t* buf = nullptr;
    size_t   buf_size = 0;
    size_t   written = 0;
    bool     overflowed = false;
};

static size_t curlWriteToBuffer(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<CurlReadContext*>(userdata);
    size_t total = size * nmemb;
    size_t remaining = ctx->buf_size > ctx->written ? ctx->buf_size - ctx->written : 0;
    size_t to_copy = std::min(total, remaining);
    if (to_copy > 0) {
        std::memcpy(ctx->buf + ctx->written, ptr, to_copy);
        ctx->written += to_copy;
    }
    if (to_copy < total) {
        ctx->overflowed = true;
    }
    return total;
}

struct HeaderParseCtx {
    uint64_t total_size = 0;
    bool found = false;
};

static size_t curlParseHeader(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<HeaderParseCtx*>(userdata);
    std::string header(static_cast<const char*>(ptr), size * nmemb);

    std::string lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lower.find("content-range") != std::string::npos) {
        auto slash = header.find('/');
        if (slash != std::string::npos) {
            std::string total_str = header.substr(slash + 1);
            while (!total_str.empty() &&
                   (total_str.back() == '\r' || total_str.back() == '\n' || std::isspace((unsigned char)total_str.back()))) {
                total_str.pop_back();
            }
            if (total_str != "*") {
                ctx->total_size = std::strtoull(total_str.c_str(), nullptr, 10);
                ctx->found = ctx->total_size > 0;
            }
        }
    }

    if (!ctx->found && lower.find("content-length") != std::string::npos) {
        auto colon = header.find(':');
        if (colon != std::string::npos) {
            std::string val = header.substr(colon + 1);
            while (!val.empty() && std::isspace((unsigned char)val.front())) val.erase(val.begin());
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
            uint64_t n = std::strtoull(val.c_str(), nullptr, 10);
            if (n > 0) ctx->total_size = n;
        }
    }

    return size * nmemb;
}
#endif

RemoteDataSource::RemoteDataSource(const std::string& base_url)
    : base_url_(base_url) {
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

RemoteDataSource::~RemoteDataSource() {
    close();
#if TSNX_HAVE_CURL
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
#endif
}

std::string RemoteDataSource::urlEncode(const std::string& value) const {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string RemoteDataSource::buildStreamUrl(uint64_t /*offset*/, uint64_t /*length*/) const {
    const std::string route = stream_route_.empty() ? torrent_hash_ : stream_route_;
    return base_url_ + "/stream/" + urlEncode(route) +
           "?link=" + urlEncode(torrent_hash_) +
           "&index=" + std::to_string(file_index_) +
           "&play";
}

std::vector<std::string> RemoteDataSource::buildStreamUrlCandidates(uint64_t offset, uint64_t length) const {
    (void)offset;
    (void)length;

    std::vector<std::string> urls;
    urls.push_back(buildStreamUrl(offset, length));

    const std::string hash_route = base_url_ + "/stream/" + urlEncode(torrent_hash_) +
                                   "?link=" + urlEncode(torrent_hash_) +
                                   "&index=" + std::to_string(file_index_) +
                                   "&play";
    urls.push_back(hash_route);

    const std::string legacy = base_url_ + "/stream?link=" + urlEncode(torrent_hash_) +
                               "&index=" + std::to_string(file_index_) +
                               "&play";
    urls.push_back(legacy);

    const std::string local_proxy = base_url_ + "/stream?infohash=" + urlEncode(torrent_hash_) +
                                    "&file_index=" + std::to_string(file_index_);
    urls.push_back(local_proxy);

    std::vector<std::string> unique;
    for (const auto& u : urls) {
        if (std::find(unique.begin(), unique.end(), u) == unique.end()) {
            unique.push_back(u);
        }
    }
    return unique;
}

bool RemoteDataSource::open(const std::string& torrent_hash, int file_index) {
    close();

    torrent_hash_ = torrent_hash;
    file_index_ = file_index;
    file_size_ = 0;
    stream_route_ = torrent_hash_;

    if (torrent_hash_.empty() || file_index_ < 0) {
        util::logLine("remote_ds: invalid open params hash/index");
        return false;
    }

    if (!resolveFileSize()) {
        util::logLine("remote_ds: failed to resolve file size, continuing without it");
    }

    opened_ = true;
    last_failure_ = ReadFailureKind::None;
    util::logLine("remote_ds: stream opened, hash=" + torrent_hash_ +
                  " index=" + std::to_string(file_index_) +
                  " size=" + std::to_string(file_size_));
    return true;
}

bool RemoteDataSource::resolveFileSize() {
#if TSNX_HAVE_CURL
    auto urls = buildStreamUrlCandidates(0, 0);
    CURL* curl = getReusableCurl(curl_handle_);
    if (!curl) return false;

    for (const auto& url : urls) {
        curl_easy_reset(curl);

        HeaderParseCtx hdr_ctx;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::min(timeout_sec_, 10)));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Range: bytes=0-0");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlParseHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr_ctx);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res == CURLE_OK && hdr_ctx.found) {
            file_size_ = hdr_ctx.total_size;
            return file_size_ > 0;
        }

        if (res == CURLE_OK) {
            double cl = 0;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
            if (cl > 0) {
                file_size_ = static_cast<uint64_t>(cl);
                return true;
            }
        }
    }
    return file_size_ > 0;
#else
    return false;
#endif
}

bool RemoteDataSource::isLikelyLocalProxy() const {
    const std::string lower = toLowerCopy(base_url_);
    return lower.find("127.0.0.1") != std::string::npos ||
           lower.find("localhost") != std::string::npos;
}

size_t RemoteDataSource::readSingleRange(uint64_t offset, void* buf, size_t size) {
    if (!opened_ || !buf || size == 0) return 0;
    last_failure_ = ReadFailureKind::None;
    if (file_size_ > 0 && offset >= file_size_) return 0;

    const size_t requested_size = expectedRangeSize(file_size_, offset, size);
    if (requested_size == 0) return 0;
    const uint64_t range_end = offset + requested_size - 1;

#if TSNX_HAVE_CURL
    auto urls = buildStreamUrlCandidates(offset, requested_size);
    bool saw_timeout = false;
    bool saw_404 = false;
    bool saw_no_response = false;
    CURL* curl = getReusableCurl(curl_handle_);
    if (!curl) {
        last_failure_ = ReadFailureKind::Other;
        return 0;
    }

    for (int attempt = 0; attempt < max_retries_; ++attempt) {
        for (const auto& url : urls) {
            curl_easy_reset(curl);

            CurlReadContext ctx;
            ctx.buf = static_cast<uint8_t*>(buf);
            ctx.buf_size = requested_size;
            ctx.written = 0;
            ctx.overflowed = false;
            RangeResponseInfo range_info{};

            std::ostringstream range_hdr;
            range_hdr << "Range: bytes=" << offset << "-" << range_end;

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, range_hdr.str().c_str());
            headers = curl_slist_append(headers, "Connection: keep-alive");
            headers = curl_slist_append(headers, "Accept-Encoding: identity");

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToBuffer);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlParseRangeHeader);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &range_info);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::min(timeout_sec_, 10)));
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
            curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
            curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 10L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);

            CURLcode res = curl_easy_perform(curl);

            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_slist_free_all(headers);

            if (http_code == 404) {
                saw_404 = true;
                last_failure_ = ReadFailureKind::NotFound404;
                util::logLine("remote_ds: stream returned 404, switching candidate");
                return 0;
            }

            if (res == CURLE_OK) {
                std::string reason;
                if (validateRangeResponse(range_info, http_code, offset, requested_size,
                                          ctx.written, ctx.overflowed, file_size_, reason)) {
                    if (file_size_ == 0 && range_info.has_total_size) {
                        file_size_ = range_info.total_size;
                    }
                    last_failure_ = ReadFailureKind::None;
                    return ctx.written;
                }
                util::logLine("remote_ds: invalid range response at offset=" +
                              std::to_string(offset) + " size=" + std::to_string(requested_size) +
                              ": " + reason);
            }

            if (res == CURLE_OPERATION_TIMEDOUT) {
                saw_timeout = true;
            } else if (res != CURLE_OK || http_code == 0) {
                saw_no_response = true;
            }
        }

        if (attempt < max_retries_ - 1) {
            util::logLine("remote_ds: retry " + std::to_string(attempt + 1) +
                          "/" + std::to_string(max_retries_) +
                          " offset=" + std::to_string(offset));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    if (saw_timeout) {
        last_failure_ = ReadFailureKind::Timeout;
    } else if (saw_no_response) {
        last_failure_ = ReadFailureKind::NoResponse;
    } else if (saw_404) {
        last_failure_ = ReadFailureKind::NotFound404;
    } else {
        last_failure_ = ReadFailureKind::Other;
    }
    util::logLine("remote_ds: read failed after " + std::to_string(max_retries_) + " attempts");
    return 0;
#else
    net::HttpClient http;
    http.setTimeout(timeout_sec_);

    std::ostringstream range_hdr;
    range_hdr << "Range: bytes=" << offset << "-" << range_end;
    std::vector<std::string> headers{
        range_hdr.str(),
        "Accept-Encoding: identity"
    };

    auto urls = buildStreamUrlCandidates(offset, requested_size);
    bool saw_404 = false;
    bool saw_no_response = false;
    for (const auto& url : urls) {
        auto resp = http.httpGet(url, headers);
        if (resp.status_code == 404) {
            saw_404 = true;
            break;
        }

        if (resp.status_code != 0) {
            RangeResponseInfo range_info{};
            std::istringstream header_stream(resp.headers);
            std::string header_line;
            while (std::getline(header_stream, header_line)) {
                parseRangeHeaderLine(header_line, range_info);
            }

            std::string reason;
            if (validateRangeResponse(range_info, resp.status_code, offset, requested_size,
                                      resp.body.size(), false, file_size_, reason)) {
                if (file_size_ == 0 && range_info.has_total_size) {
                    file_size_ = range_info.total_size;
                }
                std::memcpy(buf, resp.body.data(), resp.body.size());
                last_failure_ = ReadFailureKind::None;
                return resp.body.size();
            }

            util::logLine("remote_ds: invalid socket range response at offset=" +
                          std::to_string(offset) + " size=" + std::to_string(requested_size) +
                          ": " + reason);
        }

        if (resp.status_code == 0) {
            saw_no_response = true;
        }
    }

    if (saw_404) {
        last_failure_ = ReadFailureKind::NotFound404;
    } else if (saw_no_response) {
        last_failure_ = ReadFailureKind::NoResponse;
    } else {
        last_failure_ = ReadFailureKind::Other;
    }
    return 0;
#endif
}

size_t RemoteDataSource::read(uint64_t offset, void* buf, size_t size) {
    if (!opened_ || !buf || size == 0) return 0;
    if (file_size_ > 0 && offset >= file_size_) return 0;

    const size_t requested_size = expectedRangeSize(file_size_, offset, size);
    if (requested_size == 0) return 0;

    if (!isLikelyLocalProxy() || requested_size <= kLocalProxyRangeSlice) {
        return readSingleRange(offset, buf, requested_size);
    }

    size_t total_read = 0;
    auto* out = static_cast<uint8_t*>(buf);
    while (total_read < requested_size) {
        const size_t slice_size = std::min(kLocalProxyRangeSlice, requested_size - total_read);
        const size_t got = readSingleRange(offset + total_read, out + total_read, slice_size);
        if (got == 0) {
            if (total_read > 0) {
                util::logLine("remote_ds: local proxy partial advance offset=" + std::to_string(offset) +
                              " requested=" + std::to_string(requested_size) +
                              " got=" + std::to_string(total_read));
                last_failure_ = ReadFailureKind::None;
                return total_read;
            }
            return 0;
        }

        total_read += got;
        if (got < slice_size) {
            util::logLine("remote_ds: local proxy short slice offset=" + std::to_string(offset + total_read - got) +
                          " requested=" + std::to_string(slice_size) +
                          " got=" + std::to_string(got));
            last_failure_ = ReadFailureKind::None;
            return total_read;
        }
    }

    last_failure_ = ReadFailureKind::None;
    return total_read;
}

uint64_t RemoteDataSource::totalSize() const {
    return file_size_;
}

bool RemoteDataSource::isAvailable() const {
    net::HttpClient http;
    std::string url = base_url_ + "/echo";
    auto resp = http.httpGet(url);
    return resp.status_code == 200;
}

bool RemoteDataSource::shouldFallbackOnReadFailure() const {
    return last_failure_ == ReadFailureKind::NotFound404 ||
           last_failure_ == ReadFailureKind::Timeout ||
           last_failure_ == ReadFailureKind::NoResponse;
}

void RemoteDataSource::close() {
    opened_ = false;
    torrent_hash_.clear();
    stream_route_.clear();
    file_index_ = -1;
    file_size_ = 0;
    last_failure_ = ReadFailureKind::None;
}

} // namespace datasource
