#pragma once

#include <cstddef>
#include <cstdint>
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
};

class CustomEngineClient {
public:
    static CustomEngineClient& instance();

    bool isEnabled() const { return true; }
    const std::string& lastError() const { return last_error_; }

    bool probeFiles(const std::string& info_hash,
                    const std::string& magnet_link,
                    const std::string& torrent_file_path,
                    std::vector<CustomEngineFileInfo>& out_files,
                    std::string* err = nullptr);

    CustomEngineProbeStatus probeStatus() const;
    void cancelProbe();

private:
    CustomEngineClient() = default;
    ~CustomEngineClient();
    CustomEngineClient(const CustomEngineClient&) = delete;
    CustomEngineClient& operator=(const CustomEngineClient&) = delete;

    bool ensureEngine();
    void shutdown();

    tsnx_engine* engine_ = nullptr;
    std::string last_error_;
    bool probing_ = false;
};

} // namespace datasource
