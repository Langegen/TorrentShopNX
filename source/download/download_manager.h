#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
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

// Прогресс/результат фонового копирования не-игрового файла в downloads/.
// Поля, кроме written/total/failed/done, пишутся воркером ДО store() в атомарный
// флаг, поэтому их чтение из progress-треда корректно (happens-before).
struct FileDownloadState {
    std::atomic<unsigned long long> written{0};
    std::atomic<unsigned long long> total{0};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::string error;
    std::string dest;              // итоговый путь на диске
    unsigned long long size = 0;   // размер исходного файла
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
    int known_peers = 0;
    int dht = 0;

    // Гибридный инсталлятор (новый режим)
    std::shared_ptr<installer::HybridNspInstaller> hybrid_installer;
    std::string torrent_hash;  // Хеш торрента для DataSource

    // Асинхронный open
    std::shared_ptr<std::future<bool>> open_future;
    std::shared_ptr<std::future<bool>> start_future;
    std::shared_ptr<std::atomic<bool>> open_done;
    std::shared_ptr<std::atomic<bool>> open_success;
    std::shared_ptr<std::atomic<bool>> cancel_flag = std::make_shared<std::atomic<bool>>(false);

    // Старый streaming install (для совместимости)
    bool stream_ready = false;
    bool stream_done = false;
    std::string stream_name;
    unsigned long long install_total = 0;
    unsigned long long install_written = 0;
    float install_speed_kbps = 0.0f;
    std::chrono::steady_clock::time_point install_speed_sample_at{};
    unsigned long long last_install_written = 0;
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
    std::string cover_url;

    // Скачивание не-игрового файла (простое копирование файла в downloads/,
    // только локальный движок). Состояние живёт в фоновом воркере; progress-тред
    // только опрашивает будущее и атомарный прогресс.
    bool file_dl_dispatched = false;
    std::shared_ptr<std::future<void>> file_dl_worker;
    std::shared_ptr<std::atomic<bool>> file_dl_cancel = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<FileDownloadState> file_dl_state;
    std::string file_dl_dest;   // итоговый путь (для логов/UI)
    bool is_homebrew = false;   // Homebrew/порт: скачивается как файл(ы) в downloads/ без установки
};

class DownloadManager {
public:
    DownloadManager();
    ~DownloadManager();

    size_t addToQueue(const std::string& title,
                      const std::string& magnet,
                      int forced_file_index = -1,
                      const std::string& forced_stream_name = "",
                      bool is_homebrew = false);
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

    using ProgressCallback = std::function<void()>;
    void setProgressCallback(ProgressCallback cb) { progress_callback_ = cb; }

private:
    struct StreamConsumer {
        int torrent_id = -1;
        std::shared_ptr<std::atomic<bool>> stop;
        std::thread worker;
    };

    void ensureStreamConsumer(DownloadItem& item);
    void stopStreamConsumer(int torrent_id);
    void stopAllStreamConsumers();

    // Не-игровые файлы: скачивание в downloads/ через локальный движок.
    void handleFileDownload(size_t index, const std::vector<torrent::TorrentInfo>& list,
                            std::chrono::steady_clock::time_point now);

    std::vector<DownloadItem> queue_;
    std::unique_ptr<torrent::TorrentManager> torrent_;
    datasource::DataSourceManager ds_manager_;
    std::vector<torrent::TorrentInfo> last_torrent_list_;
    std::chrono::steady_clock::time_point last_torrent_list_poll_{};
    std::mutex stream_consumers_mtx_;
    std::vector<StreamConsumer> stream_consumers_;

    torrent::TorrentManager* getTorrent();
    bool isLocalBackend() const;

    std::thread progress_thread_;
    std::atomic<bool> progress_thread_running_{false};
    ProgressCallback progress_callback_ = nullptr;

    // Used to wake the progress thread early when an async open completes
    std::mutex progress_cv_mutex_;
    std::condition_variable progress_cv_;
    std::atomic<bool> has_open_pending_{false};  // true while any item is in StreamPreparing
};

} // namespace download
