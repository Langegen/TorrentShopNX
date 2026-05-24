#include "external_torrserver_backend.h"

#include "../utils/log.h"

namespace datasource {

ExternalTorrServerBackend::ExternalTorrServerBackend(const BackendConfig& cfg)
    : cfg_(cfg)
    , remote_(cfg.remote_url) {
    remote_.setTimeout(cfg_.timeout_sec);
    remote_.setRetryCount(cfg_.retry_count);
    status_.state = StreamState::Idle;
}

bool ExternalTorrServerBackend::open(const ContentRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.detail.clear();
    status_.state = StreamState::FetchingMetadata;
    status_.stall_count = 0;
    status_.last_offset = 0;
    status_.last_size = 0;

    if (request.info_hash.empty()) {
        status_.state = StreamState::Error;
        status_.detail = "invalid request for ExternalTorrServerBackend";
        return false;
    }

    remote_.setTimeout(cfg_.timeout_sec);
    remote_.setRetryCount(cfg_.retry_count);
    if (!remote_.open(request.info_hash, request.file_index)) {
        status_.state = StreamState::Error;
        status_.detail = "external stream open failed";
        return false;
    }

    status_.state = StreamState::StreamingOrInstalling;
    status_.total_size = remote_.totalSize();
    opened_ = true;
    util::logLine("backend/external: open hash=" + request.info_hash +
                  " file_index=" + std::to_string(request.file_index));
    return true;
}

bool ExternalTorrServerBackend::prebuffer(std::int64_t offset, std::int64_t size) {
    (void)offset;
    (void)size;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        return false;
    }
    // External TorrServer controls its own preload logic.
    return true;
}

std::int64_t ExternalTorrServerBackend::read(std::int64_t offset, void* buffer, std::int64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || offset < 0 || size <= 0 || buffer == nullptr) {
        return 0;
    }

    const size_t got = remote_.read(static_cast<uint64_t>(offset), buffer, static_cast<size_t>(size));
    status_.last_offset = static_cast<uint64_t>(offset);
    status_.last_size = static_cast<size_t>(size);
    status_.total_size = remote_.totalSize();

    if (got > 0) {
        status_.state = StreamState::StreamingOrInstalling;
        status_.stall_count = 0;
        return static_cast<std::int64_t>(got);
    }

    if (remote_.lastReadFailure() != ReadFailureKind::None) {
        status_.state = StreamState::Stalled;
        status_.stall_count += 1;
    }
    return 0;
}

BackendStatus ExternalTorrServerBackend::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool ExternalTorrServerBackend::isAvailable() const {
    return remote_.isAvailable();
}

ReadFailureKind ExternalTorrServerBackend::lastReadFailure() const {
    return remote_.lastReadFailure();
}

bool ExternalTorrServerBackend::shouldFallbackOnReadFailure() const {
    return remote_.shouldFallbackOnReadFailure();
}

void ExternalTorrServerBackend::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_.close();
    opened_ = false;
    if (status_.state != StreamState::Error) {
        status_.state = StreamState::Stopping;
    }
}

} // namespace datasource
