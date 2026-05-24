#include "backend_data_source.h"

#include "../utils/log.h"

namespace datasource {

BackendDataSource::BackendDataSource(BackendType backend_type, const BackendConfig& cfg)
    : backend_type_(backend_type)
    , cfg_(cfg) {
    ensureBackend();
}

void BackendDataSource::setConfig(const BackendConfig& cfg) {
    cfg_ = cfg;
    backend_.reset();
    ensureBackend();
}

void BackendDataSource::setTorrentContext(const std::string& info_hash,
                                          const std::string& magnet_link,
                                          const std::string& torrent_file_path) {
    if (!info_hash.empty()) request_.info_hash = info_hash;
    if (!magnet_link.empty()) request_.magnet_link = magnet_link;
    if (!torrent_file_path.empty()) request_.torrent_file_path = torrent_file_path;
}

bool BackendDataSource::open(const std::string& torrent_hash, int file_index) {
    ensureBackend();
    if (!backend_) {
        return false;
    }

    request_.file_index = file_index;
    if (!torrent_hash.empty()) {
        request_.info_hash = torrent_hash;
    }

    if (!backend_->open(request_)) {
        opened_ = false;
        return false;
    }

    const BackendStatus st = backend_->status();
    total_size_ = st.total_size;
    opened_ = true;
    return true;
}

size_t BackendDataSource::read(uint64_t offset, void* buf, size_t size) {
    if (!opened_ || !backend_ || buf == nullptr || size == 0) {
        return 0;
    }
    const std::int64_t got = backend_->read(static_cast<std::int64_t>(offset), buf, static_cast<std::int64_t>(size));
    if (got <= 0) {
        return 0;
    }
    const BackendStatus st = backend_->status();
    total_size_ = st.total_size;
    return static_cast<size_t>(got);
}

uint64_t BackendDataSource::totalSize() const {
    if (backend_) {
        return backend_->status().total_size;
    }
    return total_size_;
}

bool BackendDataSource::isAvailable() const {
    if (!backend_) {
        return false;
    }
    return backend_->isAvailable();
}

void BackendDataSource::notifyInstallInfoParsed(uint64_t offset, size_t size) {
    if (!backend_) {
        return;
    }
    if (!backend_->prebuffer(static_cast<std::int64_t>(offset), static_cast<std::int64_t>(size))) {
        util::logLine("backend-ds: prebuffer request rejected");
    }
}

void BackendDataSource::notifyStreamingComplete(bool success) {
    if (backend_) {
        backend_->notifyStreamingComplete(success);
    }
}

size_t BackendDataSource::streamPieceSize() const {
    if (!backend_) {
        return 0;
    }
    const int piece_size = backend_->pieceSize();
    return piece_size > 0 ? static_cast<size_t>(piece_size) : 0;
}

uint64_t BackendDataSource::streamTorrentOffset() const {
    if (!backend_) {
        return 0;
    }
    return backend_->fileOffsetInTorrent();
}

ReadFailureKind BackendDataSource::lastReadFailure() const {
    if (!backend_) {
        return ReadFailureKind::None;
    }
    return backend_->lastReadFailure();
}

bool BackendDataSource::shouldFallbackOnReadFailure() const {
    if (!backend_) {
        return false;
    }
    return backend_->shouldFallbackOnReadFailure();
}

SourceType BackendDataSource::type() const {
    return backend_type_ == BackendType::ExternalTorrServer
               ? SourceType::Remote
               : SourceType::LocalInternal;
}

void BackendDataSource::close() {
    if (backend_) {
        backend_->close();
    }
    opened_ = false;
}

void BackendDataSource::ensureBackend() {
    if (backend_) {
        return;
    }
    backend_ = create_backend(backend_type_, cfg_);
}

} // namespace datasource
