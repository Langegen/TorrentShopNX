#pragma once

#include "i_data_source.h"

#include <cstdint>
#include <memory>
#include <string>

namespace datasource {

enum class BackendType {
    ExternalTorrServer,
    LocalLibtorrent
};

enum class StreamState {
    Idle,
    FetchingMetadata,
    FileSelected,
    PrebufferInstallInfo,
    InstallInfoParsed,
    MainBuffering,
    StreamingOrInstalling,
    Stalled,
    Completed,
    Stopping,
    Error
};

struct ContentRequest {
    std::string info_hash;
    std::string magnet_link;
    std::string torrent_file_path;
    int file_index = -1;
};

struct BackendStatus {
    StreamState state = StreamState::Idle;
    std::string detail;
    uint64_t total_size = 0;
    uint64_t last_offset = 0;
    size_t last_size = 0;
    int stall_count = 0;
    int urgent_start = -1;
    int urgent_end = -1;
    int readahead_start = -1;
    int readahead_end = -1;
    int tail_start = -1;
    int tail_end = -1;
    int peers = 0;
    int seeds = 0;
    int known_peers = 0;
    int connect_candidates = 0;
};

struct BackendConfig {
    std::string remote_url = "http://127.0.0.1:8090";
    int local_port = 8080;
    int timeout_sec = 30;
    int retry_count = 6;
    int urgent_pieces = 4;
    int readahead_pieces = 24;
    int tail_pieces = 6;
    int stalled_extra_urgent = 2;
    int stalled_extra_readahead = 16;
    int cache_max_pieces = 512;
};

class IContentBackend {
public:
    virtual ~IContentBackend() = default;

    virtual bool open(const ContentRequest& request) = 0;
    virtual bool prebuffer(std::int64_t offset, std::int64_t size) = 0;
    virtual std::int64_t read(std::int64_t offset, void* buffer, std::int64_t size) = 0;
    virtual BackendStatus status() const = 0;
    virtual bool isAvailable() const = 0;
    virtual void notifyStreamingComplete(bool /*success*/) {}
    virtual int pieceSize() const { return 0; }
    virtual uint64_t fileOffsetInTorrent() const { return 0; }
    virtual ReadFailureKind lastReadFailure() const { return ReadFailureKind::None; }
    virtual bool shouldFallbackOnReadFailure() const { return false; }

    /// Уведомление о смене фазы (для логирования / отладки).
    virtual void onPhaseTransition(StreamState /*from*/, StreamState /*to*/) {}

    /// Текущая скорость загрузки в KB/s (-1 = неизвестно).
    virtual int downloadSpeedKBps() const { return -1; }
    virtual BackendType type() const = 0;
    virtual void close() = 0;
};

const char* streamStateName(StreamState state);

std::unique_ptr<IContentBackend> create_backend(BackendType type, const BackendConfig& cfg);

} // namespace datasource
