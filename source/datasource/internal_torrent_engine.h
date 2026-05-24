#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace datasource {

struct InternalTorrentFileInfo {
    int index = -1;
    std::string name;
    unsigned long long size = 0;
    bool wanted = true;
};

struct InternalTorrentProbeStatus {
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

class InternalTorrentEngine {
public:
    static InternalTorrentEngine& instance();

    bool probeFiles(const std::string& info_hash,
                    const std::string& magnet_link,
                    const std::string& torrent_file_path,
                    std::vector<InternalTorrentFileInfo>& out_files,
                    std::string* out_error = nullptr);

    bool isEnabled() const;
    const std::string& lastError() const;
    InternalTorrentProbeStatus probeStatus() const;
    void cancelProbe();

private:
    InternalTorrentEngine() = default;
    ~InternalTorrentEngine() = default;
    InternalTorrentEngine(const InternalTorrentEngine&) = delete;
    InternalTorrentEngine& operator=(const InternalTorrentEngine&) = delete;

    std::string last_error_;
};

} // namespace datasource
