#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace datasource {

enum class SourceType {
    Remote,
    LocalInternal,
    File
};

enum class ReadFailureKind {
    None,
    NotFound404,
    Timeout,
    NoResponse,
    Other
};

class IDataSource {
public:
    virtual ~IDataSource() = default;

    // Optional context for sources that need magnet/.torrent metadata.
    virtual void setTorrentContext(const std::string& /*info_hash*/,
                                   const std::string& /*magnet_link*/,
                                   const std::string& /*torrent_file_path*/) {}

    virtual bool open(const std::string& torrent_hash, int file_index) = 0;
    virtual size_t read(uint64_t offset, void* buf, size_t size) = 0;
    virtual uint64_t totalSize() const = 0;
    virtual bool isAvailable() const = 0;
    virtual void notifyInstallInfoParsed(uint64_t /*offset*/, size_t /*size*/) {}
    virtual void notifyStreamingComplete(bool /*success*/) {}
    virtual size_t streamPieceSize() const { return 0; }
    virtual uint64_t streamTorrentOffset() const { return 0; }
    virtual int downloadSpeedKBps() const { return -1; }

    virtual ReadFailureKind lastReadFailure() const { return ReadFailureKind::None; }
    virtual bool shouldFallbackOnReadFailure() const { return false; }

    virtual SourceType type() const = 0;
    virtual void close() = 0;
};

} // namespace datasource
