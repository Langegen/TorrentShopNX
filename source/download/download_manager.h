#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>

#include "../installer/stream_installer.h"
#include "../installer/hybrid_nsp_installer.h"
#include "../torrent/torrent_manager.h"
#include "../datasource/data_source_manager.h"

namespace download {

enum class DownloadState {
    Queued,
    Downloading,
    StreamPreparing,     // Новый: асинхронная подготовка стрима
    StreamInstalling,    // Новый: гибридная потоковая установка через NCM
    Installing,          // Совместимость: старый режим
    Completed,
    Cancelled,
    Failed,              // Новый: ошибка
    Paused
};

struct DownloadItem {
    std::string title;
    std::string magnet;
    int forced_file_index = -1;
    std::string forced_stream_name;
    int torrent_id = -1;
    DownloadState state = DownloadState::Queued;
    float progress = 0.0f;
    float install_progress = 0.0f;
    float download_speed_kbps = 0.0f;
    std::chrono::steady_clock::time_point speed_sample_at{};
    std::chrono::steady_clock::time_point start_time{};
    int seeds = 0;
    int peers = 0;

    // Гибридный инсталлятор (новый режим)
    std::unique_ptr<installer::HybridNspInstaller> hybrid_installer;
    std::string torrent_hash;  // Хеш торрента для DataSource

    // Асинхронный open
    std::shared_ptr<std::future<bool>> open_future;

    // Старый streaming install (для совместимости)
    bool stream_ready = false;
    bool stream_done = false;
    std::string stream_name;
    unsigned long long install_total = 0;
    unsigned long long install_written = 0;
    struct StreamFile {
        std::string path;
        uint64_t offset = 0;
    };
    std::vector<StreamFile> stream_files;
    size_t stream_index = 0;
    installer::StreamInstaller installer;
    bool preload_started = false;
    int preload_file_index = -1;
    std::string preload_stream_name;
    bool auto_hybrid_started = false;
    bool stream_consumer_started = false;
    uint64_t pump_offset = 0;
    int pump_zero_reads = 0;
    std::chrono::steady_clock::time_point pump_last_at{};

    // Сообщение об ошибке
    std::string error_message;

    std::string topic_id;
    std::vector<int> selected_files;
    bool priorities_set = false;
};

class DownloadManager {
public:
    DownloadManager();
    ~DownloadManager();

    size_t addToQueue(const std::string& title,
                      const std::string& magnet,
                      int forced_file_index = -1,
                      const std::string& forced_stream_name = "");
    bool startDownload(size_t index);
    void startNextDownload();
    void trackProgress();
    void shutdown();
    bool cancelDownload(size_t index);
    bool getTorrentFiles(size_t index, std::vector<torrent::TorrentFileInfo>& out_files);
    bool setFileWanted(size_t index, int file_index, bool wanted);
    bool probeTorrentFiles(const std::string& magnet,
                           std::vector<torrent::TorrentFileInfo>& out_files,
                           std::string* out_error = nullptr);
    bool hasActiveTransfers() const;

    /// Начать гибридную установку для указанного элемента очереди
    bool startHybridInstall(size_t index);

    /// Доступ к менеджеру источников данных
    datasource::DataSourceManager& dataSourceManager() { return ds_manager_; }

    const std::vector<DownloadItem>& queue() const { return queue_; }

private:
    struct StreamConsumer {
        int torrent_id = -1;
        std::shared_ptr<std::atomic<bool>> stop;
        std::thread worker;
    };

    void ensureStreamConsumer(DownloadItem& item);
    void stopStreamConsumer(int torrent_id);
    void stopAllStreamConsumers();

    std::vector<DownloadItem> queue_;
    std::unique_ptr<torrent::TorrentManager> torrent_;
    datasource::DataSourceManager ds_manager_;
    std::vector<torrent::TorrentInfo> last_torrent_list_;
    std::chrono::steady_clock::time_point last_torrent_list_poll_{};
    std::mutex stream_consumers_mtx_;
    std::vector<StreamConsumer> stream_consumers_;

    torrent::TorrentManager* getTorrent();
};

} // namespace download
