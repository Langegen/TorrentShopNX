#include "internal_torrent_engine.h"

#include "../torrent/torrent_engine.h"
#include "../utils/log.h"

namespace datasource {

InternalTorrentEngine& InternalTorrentEngine::instance() {
    static InternalTorrentEngine* inst = new InternalTorrentEngine();
    return *inst;
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
    util::logLine("InternalTorrentEngine: probeFiles start, magnet_link=" + magnet_link);
    out_files.clear();
    if (out_error) out_error->clear();

    util::logLine("InternalTorrentEngine: getting TorrentEngine instance");
    auto& engine = torrent::TorrentEngine::instance();
    util::logLine("InternalTorrentEngine: TorrentEngine instance address=" + std::to_string((uintptr_t)&engine));
    std::string hash = info_hash;
    util::logLine("InternalTorrentEngine: calling beginProbe");
    engine.beginProbe(hash);

    bool ok = false;
    if (!magnet_link.empty()) {
        util::logLine("InternalTorrentEngine: calling addMagnet");
        ok = engine.addMagnet(magnet_link, &hash);
        util::logLine("InternalTorrentEngine: addMagnet finished, ok=" + std::to_string(ok) + ", hash=" + hash);
    } else if (!torrent_file_path.empty()) {
        util::logLine("InternalTorrentEngine: calling addTorrentFile");
        ok = engine.addTorrentFile(torrent_file_path, &hash);
        util::logLine("InternalTorrentEngine: addTorrentFile finished, ok=" + std::to_string(ok));
    } else if (!hash.empty()) {
        std::vector<torrent::TorrentEngineFileInfo> existing_files;
        util::logLine("InternalTorrentEngine: calling getTorrentFiles for existing hash");
        ok = engine.getTorrentFiles(hash, existing_files);
        util::logLine("InternalTorrentEngine: getTorrentFiles for existing hash finished, ok=" + std::to_string(ok));
    }

    if (!ok) {
        const std::string err = engine.lastError().empty() ? "failed to initialize torrent" : engine.lastError();
        if (out_error) *out_error = err;
        last_error_ = err;
        util::logLine("lt_engine: probe failed: " + err);
        util::logLine("InternalTorrentEngine: calling finishProbe on error");
        engine.finishProbe();
        return false;
    }

    std::vector<torrent::TorrentEngineFileInfo> files;
    util::logLine("InternalTorrentEngine: calling getTorrentFiles to fetch parsed files");
    if (!engine.getTorrentFiles(hash, files)) {
        const std::string err = engine.lastError().empty() ? "failed to fetch file list" : engine.lastError();
        if (out_error) *out_error = err;
        last_error_ = err;
        util::logLine("lt_engine: probe failed: " + err);
        util::logLine("InternalTorrentEngine: calling finishProbe on files fetch error");
        engine.finishProbe();
        return false;
    }
    util::logLine("InternalTorrentEngine: getTorrentFiles fetched count=" + std::to_string(files.size()));

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
