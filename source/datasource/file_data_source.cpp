#include "file_data_source.h"
#include "../utils/log.h"
#include <filesystem>
#include <system_error>

namespace datasource {

FileDataSource::FileDataSource(const std::string& filePath)
    : filePath_(filePath) {
}

FileDataSource::~FileDataSource() {
    close();
}

bool FileDataSource::open(const std::string& /*torrent_hash*/, int /*file_index*/) {
    close();

    std::error_code ec;
    if (!std::filesystem::exists(filePath_, ec)) {
        util::logLine("FileDataSource: file does not exist: " + filePath_);
        return false;
    }

    fileSize_ = std::filesystem::file_size(filePath_, ec);
    if (ec) {
        fileSize_ = 0;
    }

    file_ = std::fopen(filePath_.c_str(), "rb");
    if (!file_) {
        util::logLine("FileDataSource: failed to fopen " + filePath_);
        return false;
    }

    if (fileSize_ == 0) {
#if defined(_WIN32)
        if (_fseeki64(file_, 0, SEEK_END) == 0) {
            fileSize_ = static_cast<uint64_t>(_ftelli64(file_));
            _fseeki64(file_, 0, SEEK_SET);
        }
#else
        if (fseeko(file_, 0, SEEK_END) == 0) {
            fileSize_ = static_cast<uint64_t>(ftello(file_));
            fseeko(file_, 0, SEEK_SET);
        }
#endif
    }

    util::logLine("FileDataSource: opened " + filePath_ + " (" + std::to_string(fileSize_) + " bytes)");
    return true;
}

size_t FileDataSource::read(uint64_t offset, void* buf, size_t size) {
    if (!file_ || !buf || size == 0) {
        return 0;
    }
    if (cancel_flag_ && cancel_flag_->load()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(readMutex_);
    if (!file_) {
        return 0;
    }

#if defined(_WIN32)
    if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
        return 0;
    }
#else
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
        return 0;
    }
#endif

    size_t bytesRead = std::fread(buf, 1, size, file_);
    return bytesRead;
}

uint64_t FileDataSource::totalSize() const {
    return fileSize_;
}

bool FileDataSource::isAvailable() const {
    return file_ != nullptr;
}

void FileDataSource::close() {
    std::lock_guard<std::mutex> lock(readMutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

} // namespace datasource
