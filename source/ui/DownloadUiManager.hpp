#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "../download/download_manager.h"
#include "../GameData.hpp"

namespace ui {

class DownloadManager {
public:
    static DownloadManager& instance() {
        static DownloadManager inst;
        return inst;
    }

    void init();
    void shutdown();

    download::DownloadManager& getImpl() { return impl_; }

    // Adds a download to the queue and instantly saves the state to downloads.json
    void addDownload(const Game& game, const std::vector<int>& selected_files, int forced_file_index, const std::string& forced_stream_name);
    
    // Controls download states
    bool pauseDownload(const std::string& topic_id);
    bool resumeDownload(const std::string& topic_id);
    bool cancelDownload(const std::string& topic_id);
    bool deleteDownload(const std::string& topic_id); // Deletes from list if completed/failed/cancelled

    // Checks active transfers count
    int getActiveDownloadsCount() const;

    // Load/Save state
    void saveDownloads();
    void loadDownloads();

    // Progress updates callback
    using ProgressCallback = std::function<void()>;
    void setProgressCallback(ProgressCallback cb) { progress_callback_ = cb; }
    void triggerCallback() { if (progress_callback_) progress_callback_(); }

private:
    DownloadManager() = default;
    ~DownloadManager() = default;
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    download::DownloadManager impl_;
    ProgressCallback progress_callback_ = nullptr;
};

} // namespace ui
