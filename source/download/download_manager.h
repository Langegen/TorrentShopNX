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
#include "../installer/PortInstallPlanner.hpp"
#include "../torrent/torrent_manager.h"
#include "../datasource/data_source_manager.h"
#include "../datasource/custom_engine_client.h"
#include "../utils/switch_utils.h"

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
    std::mutex msg_mutex;
    std::string stage_label;
    std::string error;
    std::string dest;              // итоговый путь на диске
    unsigned long long size = 0;   // размер исходного файла
};

// Прогресс многоэтапной автоматической установки хоумбрю портов
struct PortInstallProgressState {
    std::atomic<unsigned long long> written{0};
    std::atomic<unsigned long long> total{0};
    std::atomic<float> progress{0.0f};

    // Отдельный трекинг загрузки и распаковки/установки для портов
    std::atomic<unsigned long long> dl_written{0};
    std::atomic<unsigned long long> dl_total{0};
    std::atomic<float> dl_progress{0.0f};

    std::atomic<unsigned long long> inst_written{0};
    std::atomic<unsigned long long> inst_total{0};
    std::atomic<float> inst_progress{0.0f};

    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::mutex msg_mutex;
    std::string error;
    std::string stage_label;
    std::string current_file;
    std::shared_ptr<installer::PortInstallPlan> resolved_plan;
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
    std::chrono::steady_clock::time_point finish_time{};
    int64_t elapsed_seconds = 0;
    int seeds = 0;
    int peers = 0;
    int known_peers = 0;
    int dht = 0;

    // Сохранение точных размеров загрузки и установки
    unsigned long long download_total_bytes = 0;
    unsigned long long bytes_downloaded = 0;

    // Гибридный инсталлятор (новый режим)
    std::shared_ptr<installer::HybridNspInstaller> hybrid_installer;
    std::string torrent_hash;  // Хеш торрента для DataSource
    bool sdk_mismatch = false;
    util::SdkVersion game_sdk;
    util::SdkVersion console_sdk;

    // Асинхронный зонд списка файлов (локальный движок). probeFiles() получает
    // метаданные (анонс + DHT, до десятков секунд), поэтому идёт в своём
    // потоке, а не под мьютексом очереди. Поток отсоединён и держит job сам,
    // так что удаление элемента из очереди его не ждёт.
    struct ProbeJob {
        std::atomic<bool> done{false};
        bool ok = false;
        std::vector<datasource::CustomEngineFileInfo> files;
        std::string error;
    };
    std::shared_ptr<ProbeJob> probe_job;
    std::chrono::steady_clock::time_point probe_retry_at{};

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
    std::string title_id;
    std::vector<int> selected_files;
    bool priorities_set = false;
    std::string cover_url;

    // Скачивание не-игрового файла (простое копирование файла в downloads/,
    // только локальный движок). Состояние живёт в фоновом воркере; progress-тред
    // только опрашивает будущее и атомарный прогресс.
    bool file_dl_dispatched = false;
    std::shared_ptr<std::future<void>> file_dl_worker;
    std::shared_ptr<std::atomic<bool>> file_dl_cancel = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> file_dl_pause = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<FileDownloadState> file_dl_state;
    std::string file_dl_dest;   // итоговый путь (для логов/UI)
    bool is_homebrew = false;   // Homebrew/порт: скачивается как файл(ы) в downloads/ без установки
    std::string retro_console_id;
    std::string retro_target_mode;

    // Автоматическая установка портов хоумбрю
    bool is_port_auto_install = false;
    std::shared_ptr<installer::PortInstallPlan> port_plan;
    int selected_variant_index = 0;
    std::string install_stage_label;
    std::string install_current_file;
    bool port_install_dispatched = false;
    std::shared_ptr<std::future<void>> port_install_worker;
    std::shared_ptr<std::atomic<bool>> port_install_cancel = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> port_install_pause = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<PortInstallProgressState> port_install_state;

    // Группировка и классификация компонентов раздачи
    std::string group_id;
    std::string group_title;
    std::string component_label;
    std::string component_name;
    int component_type = 0; // 0: Base, 1: Update, 2: DLC, 3: Extra/Mod, 4: Port/HB
};

class DownloadManager {
public:
    DownloadManager();
    ~DownloadManager();

    size_t addToQueue(const std::string& title,
                      const std::string& magnet,
                      int forced_file_index = -1,
                      const std::string& forced_stream_name = "",
                      bool is_homebrew = false,
                      const std::string& retro_console_id = "",
                      const std::string& retro_target_mode = "");
    size_t addPortInstall(const std::string& title,
                          const std::string& magnet,
                          std::shared_ptr<installer::PortInstallPlan> plan,
                          int variant_index);
    bool startDownload(size_t index);
    void startNextDownload();
    void trackProgress();
    void shutdown();
    bool cancelDownload(size_t index);
    bool retryDownload(size_t index);
    bool deleteFromQueue(size_t index);
    bool pauseDownload(size_t index);
    bool resumeDownload(size_t index);
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

    std::recursive_mutex& queueMutex() const { return queue_mutex_; }
    const std::vector<DownloadItem>& queue() const { return queue_; }
    std::vector<DownloadItem>& queueMutable() { return queue_; }

    using ProgressCallback = std::function<void()>;
    void setProgressCallback(ProgressCallback cb) { progress_callback_ = cb; }

private:
    struct StreamConsumer {
        int torrent_id = -1;
        std::shared_ptr<std::atomic<bool>> stop;
        std::thread worker;
    };

    // trackProgress() body, run under queue_mutex_. `list_refreshed`: the
    // engine torrent list was already polled (without the lock) this round.
    void trackProgressLocked(bool list_refreshed);
    // Starts the background probeFiles() for a local-engine item.
    void startLocalProbe(DownloadItem& item);

    void ensureStreamConsumer(DownloadItem& item);
    void stopStreamConsumer(int torrent_id);
    void stopAllStreamConsumers();

    // Не-игровые файлы: скачивание в downloads/ через локальный движок.
    void handleFileDownload(size_t index, const std::vector<torrent::TorrentInfo>& list,
                            std::chrono::steady_clock::time_point now);

    // Автоматическая многоэтапная установка портов хоумбрю
    void handlePortAutoInstall(size_t index, const std::vector<torrent::TorrentInfo>& list,
                               std::chrono::steady_clock::time_point now);

    mutable std::recursive_mutex queue_mutex_;
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

int installFilePriority(const std::string& name);
uint64_t parseTitleIdFromFileName(const std::string& name);

} // namespace download
