#include "custom_engine_client.h"

#include "../utils/log.h"

#include <engine/engine.h>

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
    engine_ = tsnx_engine_start(6882);
    if (!engine_) {
        last_error_ = "failed to start custom engine";
        return false;
    }
    return true;
}

void CustomEngineClient::shutdown() {
    if (engine_) {
        tsnx_engine_stop(engine_);
        engine_ = nullptr;
    }
}

bool CustomEngineClient::probeFiles(const std::string& info_hash,
                                    const std::string& magnet_link,
                                    const std::string& torrent_file_path,
                                    std::vector<CustomEngineFileInfo>& out_files,
                                    std::string* err) {
    out_files.clear();
    last_error_.clear();
    if (!ensureEngine()) {
        if (err) *err = last_error_;
        return false;
    }

    probing_ = true;

    char hash[41] = {0};
    bool added = false;
    if (!torrent_file_path.empty()) {
        added = tsnx_engine_add_torrent_file(engine_, torrent_file_path.c_str(), hash, sizeof(hash));
    } else if (!magnet_link.empty()) {
        added = tsnx_engine_add_magnet(engine_, magnet_link.c_str(), hash, sizeof(hash));
    } else if (!info_hash.empty()) {
        last_error_ = "info_hash alone is not supported by the custom engine";
        probing_ = false;
        if (err) *err = last_error_;
        return false;
    } else {
        last_error_ = "no magnet or torrent file provided";
        probing_ = false;
        if (err) *err = last_error_;
        return false;
    }

    if (!added) {
        last_error_ = "failed to add torrent to custom engine";
        probing_ = false;
        if (err) *err = last_error_;
        return false;
    }

    std::string h(hash);
    // Heap-backed: tsnx_file_info is ~536 bytes, so the stack array would
    // overflow the small (64 KB default) libnx pthread stacks of worker threads.
    std::vector<tsnx_file_info> files(TSNX_MAX_FILES);
    int n = tsnx_engine_get_files(engine_, h.c_str(), files.data(), TSNX_MAX_FILES);
    for (int i = 0; i < n; i++) {
        CustomEngineFileInfo fi;
        fi.index  = files[i].index;
        fi.path   = files[i].path;
        fi.size   = files[i].size;
        fi.offset = files[i].offset;
        out_files.push_back(fi);
    }

    tsnx_engine_remove_torrent(engine_, h.c_str());
    probing_ = false;

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
    return st;
}

void CustomEngineClient::cancelProbe() {
    if (engine_) {
        tsnx_engine_stop(engine_);
        engine_ = nullptr;
    }
    probing_ = false;
}

} // namespace datasource
