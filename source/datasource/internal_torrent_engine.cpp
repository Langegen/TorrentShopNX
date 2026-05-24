#include "internal_torrent_engine.h"

#include "../torrent/torrent_engine.h"
#include "../utils/log.h"

namespace datasource {

InternalTorrentEngine& InternalTorrentEngine::instance() {
    static InternalTorrentEngine inst;
    return inst;
}

bool InternalTorrentEngine::isEnabled() const {
    return torrent::TorrentEngine::instance().isEnabled();
}

const std::string& InternalTorrentEngine::lastError() const {
    return last_error_;
}


bool InternalTorrentEngine::probeFiles(const std::string& info_hash,
                                       const std::string& magnet_link,
                                       const std::string& torrent_file_path,
                                       std::vector<InternalTorrentFileInfo>& out_files,
                                       std::string* out_error) {
    out_files.clear();
    if (out_error) out_error->clear();

    auto& engine = torrent::TorrentEngine::instance();
    std::string hash = info_hash;
    engine.beginProbe(hash);

    bool ok = false;
    if (!magnet_link.empty()) {
        ok = engine.addMagnet(magnet_link, &hash);
    } else if (!torrent_file_path.empty()) {
        ok = engine.addTorrentFile(torrent_file_path, &hash);
    } else if (!hash.empty()) {
        std::vector<torrent::TorrentEngineFileInfo> existing_files;
        ok = engine.getTorrentFiles(hash, existing_files);
    }

    if (!ok) {
        const std::string err = engine.lastError().empty() ? "failed to initialize torrent" : engine.lastError();
        if (out_error) *out_error = err;
        last_error_ = err;
        util::logLine("lt_engine: probe failed: " + err);
        engine.finishProbe();
        return false;
    }

    std::vector<torrent::TorrentEngineFileInfo> files;
    if (!engine.getTorrentFiles(hash, files)) {
        const std::string err = engine.lastError().empty() ? "failed to fetch file list" : engine.lastError();
        if (out_error) *out_error = err;
        last_error_ = err;
        util::logLine("lt_engine: probe failed: " + err);
        engine.finishProbe();
        return false;
    }

    out_files.reserve(files.size());
    for (const auto& file : files) {
        InternalTorrentFileInfo info;
        info.index = file.index;
        info.name = file.name;
        info.size = file.size;
        info.wanted = file.wanted;
        out_files.push_back(std::move(info));
    }

    last_error_.clear();
    engine.finishProbe();
    return !out_files.empty();
}


InternalTorrentProbeStatus InternalTorrentEngine::probeStatus() const {
    const auto status = torrent::TorrentEngine::instance().probeStatus();
    InternalTorrentProbeStatus out;
    out.active = status.active;
    out.has_metadata = status.has_metadata;
    out.session_listening = status.session_listening;
    out.peers = status.peers;
    out.seeds = status.seeds;
    out.known_peers = status.known_peers;
    out.connect_candidates = status.connect_candidates;
    out.dht_nodes = status.dht_nodes;
    out.listen_port = status.listen_port;
    out.torrent_state = status.torrent_state;
    out.phase = status.phase;
    out.hash = status.hash;
    out.detail = status.detail;
    return out;
}

void InternalTorrentEngine::cancelProbe() {
    torrent::TorrentEngine::instance().cancelProbe();
}

} // namespace datasource
