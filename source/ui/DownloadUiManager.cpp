#include "DownloadUiManager.hpp"
#include <fstream>
#include <algorithm>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"
#include "../utils/app_paths.h"
#include "../config/config.h"

#include <engine/engine.h>

namespace ui {

void DownloadManager::init() {
    auto& cfg = config::ConfigManager::instance();
    impl_.dataSourceManager().setRemoteUrl(cfg.getTorrServerUrl());
    const std::string mode = cfg.getDataMode();
    if (mode == "local_client" || mode == "custom_engine") {
        impl_.dataSourceManager().setMode(datasource::DataSourceMode::CustomEngine);
    } else {
        impl_.dataSourceManager().setMode(datasource::DataSourceMode::Remote);
    }

    impl_.setProgressCallback([this]() {
        triggerCallback();
    });

    // Load and restore previous downloads
    loadDownloads();
}

void DownloadManager::shutdown() {
    impl_.shutdown();
}

void DownloadManager::addDownload(const Game& game, const std::vector<int>& selected_files, int forced_file_index, const std::string& forced_stream_name, const std::string& retro_console_id) {
    size_t idx = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
        idx = impl_.addToQueue(game.title, game.magnet, forced_file_index, forced_stream_name, isHomebrewGame(game), retro_console_id);

        auto& item = impl_.queueMutable()[idx];
        item.topic_id = game.topic_id;
        item.selected_files = selected_files;
        item.priorities_set = false;
        item.cover_url = game.cover;
        item.is_homebrew = isHomebrewGame(game);
        item.retro_console_id = retro_console_id;

        // Start the download immediately if no transfers are active
        if (!impl_.hasActiveTransfers()) {
            impl_.startDownload(idx);
        }

        util::logLine("download_ui: added game " + game.title + " (topic_id=" + game.topic_id + ") to download queue, is_homebrew=" + (item.is_homebrew ? "true" : "false") + ", retro=" + retro_console_id);
    }
    saveDownloads();
}

bool DownloadManager::pauseDownload(const std::string& topic_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
    const auto& queue = impl_.queue();
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].topic_id == topic_id) {
            bool ok = impl_.pauseDownload(i);
            if (ok) {
                saveDownloads();
                triggerCallback();
            }
            return ok;
        }
    }
    return false;
}

bool DownloadManager::resumeDownload(const std::string& topic_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
    const auto& queue = impl_.queue();
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].topic_id == topic_id) {
            bool ok = impl_.resumeDownload(i);
            if (ok) {
                saveDownloads();
                triggerCallback();
            }
            return ok;
        }
    }
    return false;
}

bool DownloadManager::cancelDownload(const std::string& topic_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
    const auto& queue = impl_.queue();
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].topic_id == topic_id) {
            bool ok = impl_.cancelDownload(i);
            if (ok) {
                util::logLine("download_ui: cancelled topic_id=" + topic_id);
                saveDownloads();
                triggerCallback();
            }
            return ok;
        }
    }
    return false;
}

bool DownloadManager::retryDownload(const std::string& topic_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
    const auto& queue = impl_.queue();
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].topic_id == topic_id) {
            bool ok = impl_.retryDownload(i);
            if (ok) {
                util::logLine("download_ui: retried topic_id=" + topic_id);
                saveDownloads();
                triggerCallback();
            }
            return ok;
        }
    }
    return false;
}

bool DownloadManager::deleteDownload(const std::string& topic_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
    const auto& queue = impl_.queue();
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].topic_id == topic_id) {
            impl_.deleteFromQueue(i);
            util::logLine("download_ui: deleted topic_id=" + topic_id + " from queue");
            saveDownloads();
            triggerCallback();
            return true;
        }
    }
    return false;
}

int DownloadManager::getActiveDownloadsCount() const {
    std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
    int count = 0;
    for (const auto& item : impl_.queue()) {
        if (item.state == download::DownloadState::Downloading ||
            item.state == download::DownloadState::StreamPreparing ||
            item.state == download::DownloadState::StreamInstalling) {
            count++;
        }
    }
    return count;
}

void DownloadManager::saveDownloads() {
    // No-op: downloads are only kept in-memory for the current session
}

void DownloadManager::loadDownloads() {
    // Clean up old downloads.json file if present (pre-reorg layout)
    std::remove(TSNX_BASE_DIR "/downloads.json");
    std::remove(TSNX_DATA_DIR "/downloads.json");
}

} // namespace ui
