#pragma once

#include "i_data_source.h"
#include <string>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>

namespace datasource {

class FileDataSource : public IDataSource {
public:
    explicit FileDataSource(const std::string& filePath);
    ~FileDataSource() override;

    bool open(const std::string& torrent_hash = "", int file_index = 0) override;
    size_t read(uint64_t offset, void* buf, size_t size) override;
    uint64_t totalSize() const override;
    bool isAvailable() const override;
    SourceType type() const override { return SourceType::File; }
    void close() override;

    void setCancelFlag(const std::atomic<bool>* flag) override { cancel_flag_ = flag; }
    int downloadSpeedKBps() const override { return 0; }
    int livePeers() const override { return -1; }

    const std::string& filePath() const { return filePath_; }

private:
    std::string filePath_;
    FILE* file_ = nullptr;
    uint64_t fileSize_ = 0;
    const std::atomic<bool>* cancel_flag_ = nullptr;
    std::mutex readMutex_;
};

} // namespace datasource
