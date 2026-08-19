#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <engine/engine.h>

namespace datasource {

struct CustomEngineFileInfo {
    int index = 0;
    std::string path;
    std::int64_t size = 0;
    std::int64_t offset = 0;
};

struct CustomEngineProbeStatus {
    bool active = false;
    std::string phase;
    int seeds = 0;
    int peers = 0;
    int known_peers = 0;
    int dht_nodes = 0;
    float progress = 0.0f;
    int meta_peers_tried = 0;
    int meta_peers_total = 0;
};

/*
 * Shared entry point to the custom engine. One long-lived engine instance
 * serves both the file-list probe and the download/stream backend, so a
 * torrent probed moments ago is still registered when the download starts:
 * no second metadata fetch, no "preparing for installation" stall.
 */
class CustomEngineClient {
public:
    static CustomEngineClient& instance();

    bool isEnabled() const { return true; }
    const std::string& lastError() const { return last_error_; }

    /* Shared engine for probe AND download. Starts on first use. */
    tsnx_engine* sharedEngine();

    /*
     * Fetch and return the torrent's file list. The torrent is left
     * registered in the engine (metadata-only, no download threads) so the
     * download can pick it up instantly; releaseProbeTorrent() drops it if
     * the user leaves without downloading.
     */
    bool probeFiles(const std::string& info_hash,
                    const std::string& magnet_link,
                    const std::string& torrent_file_path,
                    std::vector<CustomEngineFileInfo>& out_files,
                    std::string* err = nullptr);

    CustomEngineProbeStatus probeStatus() const;

    /* Aborts an in-flight probe's metadata fetch (does not stop the engine). */
    void cancelProbe();

    /* Removes the kept probe torrent, unless a download has adopted it. */
    void releaseProbeTorrent();

    /* Download took over the torrent: it survives probe cleanup. */
    void markInUse(const std::string& hash);
    void unmarkInUse(const std::string& hash);

private:
    CustomEngineClient() = default;
    ~CustomEngineClient();
    CustomEngineClient(const CustomEngineClient&) = delete;
    CustomEngineClient& operator=(const CustomEngineClient&) = delete;

    bool ensureEngine();
    void shutdown();

    tsnx_engine* engine_ = nullptr;
    std::string last_error_;

    mutable std::mutex probe_mtx_;
    std::atomic<bool> probe_cancel_{false};
    bool probing_ = false;

    std::mutex keep_mtx_;
    std::string kept_hash_;              // probe torrent kept for download
    std::vector<std::string> in_use_;    // hashes adopted by a download
};

} // namespace datasource
