#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <set>
#include <system_error>

namespace torrent {


struct TorrentEngineFileInfo {
    int index = -1;
    std::string name;
    unsigned long long size = 0;
    bool wanted = true;
};

struct TorrentEngineItem {
    std::string hash;
    std::string name;
    float progress = 0.0f;
    float download_speed_kbps = 0.0f;
    unsigned long long loaded_size = 0;
    unsigned long long torrent_size = 0;
    int seeds = 0;
    int peers = 0;
    int dht = 0;
};

struct TorrentEngineProbeStatus {
    bool active = false;
    bool has_metadata = false;
    bool session_listening = false;
    int peers = 0;
    int seeds = 0;
    int known_peers = 0;
    int connect_candidates = 0;
    int dht_nodes = 0;
    int listen_port = 0;
    int torrent_state = 0;
    std::string phase;
    std::string hash;
    std::string detail;
};

enum class TorrentState {
    Idle,
    FetchingMetadata,
    FileSelected,
    PrebufferInstallInfo,
    InstallInfoParsed,
    MainWindowBuffering,
    Installing,
    Stalled,
    Completed,
    Error
};

class TorrentEngine {
public:
    struct Impl;

    static TorrentEngine& instance();

    bool start(int port = 8080);
    void stop();

    bool isEnabled() const;
    bool isRunning() const;
    int port() const { return port_; }
    std::string serverUrl() const;
    const std::string& lastError() const { return last_error_; }

    bool addMagnet(const std::string& magnet, std::string* out_hash = nullptr);
    bool addTorrentFile(const std::string& torrent_file_path, std::string* out_hash = nullptr);

    bool getTorrentList(std::vector<TorrentEngineItem>& out_items);
    bool getTorrentFiles(const std::string& hash, std::vector<TorrentEngineFileInfo>& out_files);
    bool setFileWanted(const std::string& hash, int file_index, bool wanted);
    bool removeTorrent(const std::string& hash);
    bool pauseTorrent(const std::string& hash);
    bool resumeTorrent(const std::string& hash);

    bool prepareStream(const std::string& info_hash,
                       const std::string& magnet_link,
                       const std::string& torrent_file_path,
                       int file_index);
    size_t readPreparedAvailable(uint64_t offset, void* buf, size_t size);
    static void setStreamMinKeepOffset(const std::string& hash, uint64_t offset);

    void beginProbe(const std::string& hash_hint = "");
    void finishProbe();
    void cancelProbe();
    TorrentEngineProbeStatus probeStatus() const;

    // Аксессоры для LocalLibtorrentBackend.
    // Позволяют переиспользовать session/handle без создания дубликатов.
    // Возвращают void* чтобы не включать libtorrent headers.
    struct StreamAccessInfo {
        void* session_ptr = nullptr;     // std::shared_ptr<lt::session>*
        void* handle_ptr = nullptr;      // lt::torrent_handle*
        int file_index = -1;
        int64_t file_size = 0;
        int64_t file_offset = 0;
        int piece_size = 0;
        int first_piece = 0;
        int last_piece = 0;
        bool valid = false;
    };
    StreamAccessInfo getStreamAccess();

private:
    TorrentEngine();
    ~TorrentEngine();
    TorrentEngine(const TorrentEngine&) = delete;
    TorrentEngine& operator=(const TorrentEngine&) = delete;
    Impl* impl() { return impl_.get(); }
    const Impl* impl() const { return impl_.get(); }

    std::unique_ptr<Impl> impl_;
    int port_ = 8080;
    mutable std::mutex state_mutex_;
    bool server_running_ = false;
    std::string last_error_;
};

void discardMemoryStoragePiece(const std::string& hash, int piece_index);
void markMemoryStoragePieceAvailable(const std::string& hash, int piece_index);
void markMemoryStorageAllPiecesAvailable(const std::string& hash);

} // namespace torrent
