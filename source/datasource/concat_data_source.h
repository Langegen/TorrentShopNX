#pragma once

#include "i_data_source.h"

#include <string>
#include <vector>

namespace datasource {

// Presents the volumes of a split archive (data.7z.001, .002, ...) stored as
// separate files of one torrent as a single stream. The base source holds only
// one file open at a time, so a read in another volume closes the current file
// and opens the next one. Reads never cross a volume boundary (short reads are
// fine for the archive readers).
class ConcatDataSource : public IDataSource {
public:
    struct Volume {
        int file_index = -1;
        uint64_t size = 0;
    };

    ConcatDataSource(IDataSource* base, std::string hash, std::vector<Volume> volumes)
        : base_(base), hash_(std::move(hash)), volumes_(std::move(volumes)) {
        uint64_t pos = 0;
        for (const auto& v : volumes_) {
            starts_.push_back(pos);
            pos += v.size;
        }
        total_ = pos;
    }

    ~ConcatDataSource() override { close(); }

    // Opens the first volume (the arguments are ignored: volumes are fixed).
    bool open(const std::string& /*torrent_hash*/, int /*file_index*/) override {
        return !volumes_.empty() && switchTo(0);
    }

    size_t read(uint64_t offset, void* buf, size_t size) override {
        if (!base_ || size == 0 || offset >= total_) return 0;
        size_t v = volumeAt(offset);
        if (!switchTo(v)) return 0;
        const uint64_t local = offset - starts_[v];
        const uint64_t left = volumes_[v].size - local;
        const size_t want = left < size ? static_cast<size_t>(left) : size;
        return base_->read(local, buf, want);
    }

    uint64_t totalSize() const override { return total_; }
    bool isAvailable() const override { return base_ && base_->isAvailable(); }
    SourceType type() const override { return base_ ? base_->type() : SourceType::LocalInternal; }

    void close() override {
        if (base_ && current_ >= 0) base_->close();
        current_ = -1;
    }

    void setCancelFlag(const std::atomic<bool>* flag) override {
        if (base_) base_->setCancelFlag(flag);
    }
    void setSchedulerEnabled(bool enabled) override {
        if (base_) base_->setSchedulerEnabled(enabled);
    }
    void notifyStreamingComplete(bool success) override {
        if (base_ && current_ >= 0) base_->notifyStreamingComplete(success);
        current_ = -1;
    }
    int downloadSpeedKBps() const override { return base_ ? base_->downloadSpeedKBps() : -1; }
    int livePeers() const override { return base_ ? base_->livePeers() : -1; }
    ReadFailureKind lastReadFailure() const override {
        return base_ ? base_->lastReadFailure() : ReadFailureKind::None;
    }

    // How many times a volume was opened (for logs and tests).
    int volumeSwitches() const { return switches_; }

private:
    size_t volumeAt(uint64_t offset) const {
        size_t v = 0;
        while (v + 1 < starts_.size() && offset >= starts_[v + 1]) ++v;
        return v;
    }

    bool switchTo(size_t v) {
        if (static_cast<int>(v) == current_) return true;
        if (current_ >= 0) base_->close();
        current_ = -1;
        if (!base_->open(hash_, volumes_[v].file_index)) return false;
        current_ = static_cast<int>(v);
        ++switches_;
        return true;
    }

    IDataSource* base_ = nullptr;
    std::string hash_;
    std::vector<Volume> volumes_;
    std::vector<uint64_t> starts_;
    uint64_t total_ = 0;
    int current_ = -1;
    int switches_ = 0;
};

} // namespace datasource
