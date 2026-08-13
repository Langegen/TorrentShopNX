#pragma once

#include "i_content_backend.h"
#include "i_data_source.h"

#include <memory>

namespace datasource {

class BackendDataSource : public IDataSource {
public:
    BackendDataSource(BackendType backend_type, const BackendConfig& cfg);
    ~BackendDataSource() override = default;

    void setConfig(const BackendConfig& cfg);

    void setTorrentContext(const std::string& info_hash,
                           const std::string& magnet_link,
                           const std::string& torrent_file_path) override;

    void setCancelFlag(const std::atomic<bool>* flag) override {
        if (backend_) {
            backend_->setCancelFlag(flag);
        }
    }

    bool open(const std::string& torrent_hash, int file_index) override;
    size_t read(uint64_t offset, void* buf, size_t size) override;
    uint64_t totalSize() const override;
    bool isAvailable() const override;
    void notifyInstallInfoParsed(uint64_t offset, size_t size) override;
    void notifyStreamingComplete(bool success) override;
    size_t streamPieceSize() const override;
    uint64_t streamTorrentOffset() const override;
    int downloadSpeedKBps() const override;
    ReadFailureKind lastReadFailure() const override;
    bool shouldFallbackOnReadFailure() const override;
    SourceType type() const override;
    void close() override;

private:
    void ensureBackend();

    BackendType backend_type_;
    BackendConfig cfg_;
    std::unique_ptr<IContentBackend> backend_;
    ContentRequest request_;
    uint64_t total_size_ = 0;
    bool opened_ = false;
};

} // namespace datasource
