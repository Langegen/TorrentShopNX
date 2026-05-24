#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace torrent {

struct TorrentInfo {
    int id = -1;
    std::string name;
    std::string hash;  ///< Хеш торрента (для DataSource стриминга)
    float percent_done = 0.0f;
    float download_speed_kbps = 0.0f;
    unsigned long long loaded_size = 0;
    unsigned long long torrent_size = 0;
};

struct TorrentFileInfo {
    int index = -1;
    std::string name;
    unsigned long long size = 0;
    bool wanted = true;
};

class TorrentManager {
public:
    TorrentManager();
    ~TorrentManager();

    int addMagnet(const std::string& magnet);
    std::vector<TorrentInfo> getTorrentList();
    float getTorrentProgress(int id);
    bool pauseTorrent(int id);
    bool resumeTorrent(int id);
    bool cancelTorrent(int id);
    bool getStreamFiles(int id, std::vector<std::string>& out_paths, std::string& out_name);
    bool getTorrentFiles(int id, std::vector<TorrentFileInfo>& out_files);
    bool setFileWanted(int id, int file_index, bool wanted);
    bool preloadTorrentFile(int id, int file_index, const std::string& stream_name = "");
    size_t pumpStreamRead(int id, int file_index, uint64_t offset, size_t max_bytes, const std::string& stream_name = "");
    void setServerUrl(const std::string& url);
    bool isServerReachable() const { return server_reachable_; }

    /// Получить хеш торрента по локальному ID (для DataSource стриминга)
    bool getTorrentHash(int id, std::string& out_hash);

    /// Получить URL сервера TorrServer (для DataSourceManager)
    std::string getServerUrl() const { return torrserver_url_; }

private:
    struct TorrentRef {
        int local_id = -1;
        std::string remote_id;
    };

    void ensureDirs() const;
    void loadConfig();
    bool pingServer();
    int allocateLocalId();
    bool resolveRemoteId(int local_id, std::string& out_remote_id);

    std::string extractJsonValue(const std::string& obj, const std::string& key) const;
    double extractJsonNumber(const std::string& obj, const std::string& key) const;
    bool extractJsonBool(const std::string& obj, const std::string& key, bool default_value) const;

    std::vector<std::string> splitTopLevelObjects(const std::string& json) const;
    std::string buildUrl(const std::string& path) const;
    std::string urlEncode(const std::string& value) const;

    bool addMagnetViaApi(const std::string& magnet, std::string& out_remote_id);
    bool removeViaApi(const std::string& remote_id);
    bool actionViaApi(const std::string& remote_id, const std::vector<std::string>& actions);
    bool getTorrentDetails(const std::string& remote_id, std::string& out_body);
    bool parseAndCacheList(const std::string& body, std::vector<TorrentInfo>& out_list);

    std::string config_dir_;
    std::string download_dir_;
    std::string config_file_;
    std::string torrserver_url_;
    bool server_reachable_ = false;
    int next_local_id_ = 1;
    std::unordered_map<int, TorrentRef> refs_;
};

} // namespace torrent
