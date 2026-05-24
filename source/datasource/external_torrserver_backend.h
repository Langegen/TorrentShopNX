#pragma once

#include "i_content_backend.h"
#include "remote_data_source.h"

#include <mutex>

namespace datasource {

class ExternalTorrServerBackend : public IContentBackend {
public:
    explicit ExternalTorrServerBackend(const BackendConfig& cfg);

    bool open(const ContentRequest& request) override;
    bool prebuffer(std::int64_t offset, std::int64_t size) override;
    std::int64_t read(std::int64_t offset, void* buffer, std::int64_t size) override;
    BackendStatus status() const override;
    bool isAvailable() const override;
    ReadFailureKind lastReadFailure() const override;
    bool shouldFallbackOnReadFailure() const override;
    BackendType type() const override { return BackendType::ExternalTorrServer; }
    void close() override;

private:
    BackendConfig cfg_;
    RemoteDataSource remote_;
    mutable std::mutex mutex_;
    BackendStatus status_;
    bool opened_ = false;
};

} // namespace datasource

