#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include "../download/download_manager.h"
#include "../GameData.hpp"

namespace ui {

class DownloadManager {
public:
    static DownloadManager& instance() {
        static DownloadManager* inst = new DownloadManager();
        return *inst;
    }

    void init();
    void shutdown();

    download::DownloadManager& getImpl() { return impl_; }

    // Adds a download to the queue and instantly saves the state to downloads.json
    void addDownload(const Game& game, const std::vector<int>& selected_files, int forced_file_index, const std::string& forced_stream_name, const std::string& retro_console_id = "",
                     const std::string& group_id = "", const std::string& group_title = "", const std::string& component_label = "", const std::string& component_name = "", int component_type = 0,
                     unsigned long long initial_file_size = 0);
    
    // Adds a homebrew port automatic installation to the queue
    void addPortInstall(const Game& game, std::shared_ptr<installer::PortInstallPlan> plan, int variant_index,
                        bool with_optional = false);
    
    // Checks if game/torrent is already active in downloads queue
    bool hasActiveDownload(const std::string& topic_id, const std::string& magnet = "", int forced_file_index = -1) const;

    // Controls download states
    bool pauseDownload(const std::string& topic_id);
    bool resumeDownload(const std::string& topic_id);
    bool cancelDownload(const std::string& topic_id);
    bool retryDownload(const std::string& topic_id);
    bool deleteDownload(const std::string& topic_id); // Deletes from list if completed/failed/cancelled

    // Checks active transfers count
    int getActiveDownloadsCount() const;

    // Load/Save state
    void saveDownloads();
    void loadDownloads();

    // Progress updates callback
    // Set from the UI thread, called from the download thread: the callback
    // is swapped as a shared_ptr under a lock and called on a local copy, so
    // replacing it never frees the function object that is running.
    using ProgressCallback = std::function<void()>;
    void setProgressCallback(ProgressCallback cb) {
        auto p = cb ? std::make_shared<ProgressCallback>(std::move(cb)) : nullptr;
        std::lock_guard<std::mutex> lock(callback_mutex_);
        progress_callback_ = std::move(p);
    }
    void triggerCallback() {
        std::shared_ptr<ProgressCallback> cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cb = progress_callback_;
        }
        if (cb && *cb) (*cb)();
    }

private:
    DownloadManager() = default;
    ~DownloadManager() = default;
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    download::DownloadManager impl_;
    std::mutex callback_mutex_;
    std::shared_ptr<ProgressCallback> progress_callback_;
};

} // namespace ui
