#include "custom_engine_client.h"

#include "../utils/log.h"
#include "../config/config.h"

#include <engine/engine.h>
#include "../engine/torrent_meta.h"

#include <algorithm>
#include <cstring>

namespace datasource {

CustomEngineClient& CustomEngineClient::instance() {
    static CustomEngineClient inst;
    return inst;
}

CustomEngineClient::~CustomEngineClient() {
    shutdown();
}

bool CustomEngineClient::ensureEngine() {
    if (engine_) return true;
    // The configured listen port: forward it (TCP) on the router to the
    // console so firewalled seeders can dial in.
    engine_ = tsnx_engine_start(config::ConfigManager::instance().getListenPort());
    if (!engine_) {
        last_error_ = "failed to start custom engine";
        return false;
    }
    return true;
}

tsnx_engine* CustomEngineClient::sharedEngine() {
    if (!ensureEngine()) return nullptr;
    return engine_;
}

void CustomEngineClient::shutdown() {
    if (engine_) {
        tsnx_engine_stop(engine_);
        engine_ = nullptr;
    }
}

// Best-effort lowercase btih extraction so a probe can reuse an existing slot
// (kept probe torrent or active download) even when the caller did not pass
// the info hash separately.
static std::string magnetBtih(std::string magnet) {
    std::transform(magnet.begin(), magnet.end(), magnet.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string marker = "xt=urn:btih:";
    size_t pos = magnet.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    size_t end = magnet.find('&', pos);
    if (end == std::string::npos) end = magnet.size();
    if (end <= pos) return {};
    return magnet.substr(pos, end - pos);
}

bool CustomEngineClient::probeFiles(const std::string& info_hash,
                                    const std::string& magnet_link,
                                    const std::string& torrent_file_path,
                                    std::vector<CustomEngineFileInfo>& out_files,
                                    std::string* err) {
    // One probe at a time: the engine registry is not re-entrant, and both the
    // file-select view and the download progress thread can call in.
    std::lock_guard<std::mutex> lock(probe_mtx_);
    out_files.clear();
    last_error_.clear();

    tsnx_engine* eng = sharedEngine();
    if (!eng) {
        if (err) *err = last_error_;
        return false;
    }

    char hash[41] = {0};
    std::string reuse_hash = info_hash;
    if (reuse_hash.empty() && !magnet_link.empty())
        reuse_hash = magnetBtih(magnet_link);
    if (!reuse_hash.empty() && reuse_hash.size() == 40 &&
        tsnx_engine_has_torrent(eng, reuse_hash.c_str())) {
        // Already registered (kept probe slot or an active download).
        snprintf(hash, sizeof(hash), "%s", reuse_hash.c_str());
    } else {
        probing_ = true;
        probe_cancel_.store(false);
        bool added = false;
        if (!torrent_file_path.empty()) {
            added = tsnx_engine_add_torrent_file(eng, torrent_file_path.c_str(),
                                                 hash, sizeof(hash));
        } else if (!magnet_link.empty()) {
            // Metadata-only add: the probe needs the file list, not a running
            // download. The slot stays in the engine for the download to reuse.
            added = tsnx_engine_add_magnet_ex(
                eng, magnet_link.c_str(), -1, true,
                reinterpret_cast<const volatile bool*>(&probe_cancel_),
                hash, sizeof(hash));
        } else {
            last_error_ = "info_hash alone is not supported by the custom engine";
            probing_ = false;
            if (err) *err = last_error_;
            return false;
        }
        probing_ = false;

        if (!added) {
            last_error_ = "failed to add torrent to custom engine";
            if (err) *err = last_error_;
            return false;
        }

        // Keep the slot; drop the previous probe's torrent unless a download
        // has adopted it.
        std::string h(hash);
        std::lock_guard<std::mutex> keep(keep_mtx_);
        if (!kept_hash_.empty() && kept_hash_ != h &&
            std::find(in_use_.begin(), in_use_.end(), kept_hash_) == in_use_.end()) {
            tsnx_engine_remove_torrent(eng, kept_hash_.c_str());
            util::logLine("custom_engine: released previous probe torrent " + kept_hash_);
        }
        kept_hash_ = h;
    }

    std::string h(hash);
    // Heap-backed: tsnx_file_info is ~536 bytes, so the stack array would
    // overflow the small (64 KB default) libnx pthread stacks of worker threads.
    std::vector<tsnx_file_info> files(TSNX_MAX_FILES);
    int n = tsnx_engine_get_files(eng, h.c_str(), files.data(), TSNX_MAX_FILES);
    for (int i = 0; i < n; i++) {
        CustomEngineFileInfo fi;
        fi.index  = files[i].index;
        fi.path   = files[i].path;
        fi.size   = files[i].size;
        fi.offset = files[i].offset;
        out_files.push_back(fi);
    }

    if (out_files.empty()) {
        last_error_ = "torrent contains no files";
        if (err) *err = last_error_;
        return false;
    }
    return true;
}

CustomEngineProbeStatus CustomEngineClient::probeStatus() const {
    CustomEngineProbeStatus st{};
    st.active = probing_;
    st.phase  = engine_ ? "custom engine ready" : "custom engine offline";
    if (engine_) {
        tsnx_torrent_item items[8];
        int n = tsnx_engine_get_torrents(engine_, items, 8);
        if (n > 0) {
            st.seeds        = items[0].seeds;
            st.peers        = items[0].peers;
            st.known_peers  = items[0].known_peers;
            st.dht_nodes    = items[0].dht_nodes;
            st.progress     = items[0].progress;
        }
    }
    // While the probe is fetching metadata the torrent is not registered yet;
    // surface the fetch's own progress instead of a flat 0/0/0.
    st.meta_peers_tried = torrent_meta_peers_tried;
    st.meta_peers_total = torrent_meta_peers_total;
    if (st.active) {
        const char *phase = torrent_meta_state_str(torrent_meta_state);
        st.phase = phase ? phase : "fetching";
    }
    return st;
}

void CustomEngineClient::cancelProbe() {
    probe_cancel_.store(true);
}

void CustomEngineClient::releaseProbeTorrent() {
    std::lock_guard<std::mutex> lock(probe_mtx_);
    if (!engine_) return;
    std::lock_guard<std::mutex> keep(keep_mtx_);
    if (!kept_hash_.empty() &&
        std::find(in_use_.begin(), in_use_.end(), kept_hash_) == in_use_.end()) {
        tsnx_engine_remove_torrent(engine_, kept_hash_.c_str());
        util::logLine("custom_engine: released probe torrent " + kept_hash_);
        kept_hash_.clear();
    }
}

void CustomEngineClient::markInUse(const std::string& hash) {
    if (hash.empty()) return;
    std::lock_guard<std::mutex> keep(keep_mtx_);
    if (std::find(in_use_.begin(), in_use_.end(), hash) == in_use_.end())
        in_use_.push_back(hash);
}

void CustomEngineClient::unmarkInUse(const std::string& hash) {
    std::lock_guard<std::mutex> keep(keep_mtx_);
    in_use_.erase(std::remove(in_use_.begin(), in_use_.end(), hash),
                  in_use_.end());
}

} // namespace datasource
