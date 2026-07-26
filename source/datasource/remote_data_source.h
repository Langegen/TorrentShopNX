#pragma once

#include "i_data_source.h"
#include "../net/http_client.h"

#include <string>
#include <vector>

namespace datasource {

class RemoteDataSource : public IDataSource {
public:
    explicit RemoteDataSource(const std::string& base_url);
    ~RemoteDataSource() override;

    bool open(const std::string& torrent_hash, int file_index) override;
    size_t read(uint64_t offset, void* buf, size_t size) override;
    uint64_t totalSize() const override;
    bool isAvailable() const override;
    ReadFailureKind lastReadFailure() const override { return last_failure_; }
    bool shouldFallbackOnReadFailure() const override;
    SourceType type() const override { return SourceType::Remote; }
    void close() override;

    void setCancelFlag(const std::atomic<bool>* flag) override { cancel_flag_ = flag; }
    void setTimeout(int seconds) { timeout_sec_ = seconds; }
    void setRetryCount(int count) { max_retries_ = count; }

private:
    std::string buildStreamUrl(uint64_t offset, uint64_t length) const;
    std::vector<std::string> buildStreamUrlCandidates(uint64_t offset, uint64_t length) const;
    std::string urlEncode(const std::string& value) const;
    bool resolveFileSize();
    bool isLikelyLocalProxy() const;
    size_t readSingleRange(uint64_t offset, void* buf, size_t size);

    std::string base_url_;
    std::string torrent_hash_;
    std::string stream_route_;
    int         file_index_    = -1;
    uint64_t    file_size_     = 0;
    bool        opened_        = false;
    int         timeout_sec_   = 30;
    int         max_retries_   = 6;
    const std::atomic<bool>* cancel_flag_ = nullptr;
    void*       curl_handle_   = nullptr;
    ReadFailureKind last_failure_ = ReadFailureKind::None;
};

} // namespace datasource
