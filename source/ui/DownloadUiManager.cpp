#include "DownloadUiManager.hpp"
#include <fstream>
#include <algorithm>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"
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

void DownloadManager::addDownload(const Game& game, const std::vector<int>& selected_files, int forced_file_index, const std::string& forced_stream_name) {
    size_t idx = impl_.addToQueue(game.title, game.magnet, forced_file_index, forced_stream_name);

    // Access the item directly to set custom metadata
    auto& queue = const_cast<std::vector<download::DownloadItem>&>(impl_.queue());
    auto& item = queue[idx];
    item.topic_id = game.topic_id;
    item.selected_files = selected_files;
    item.priorities_set = false;

    // Start the download immediately if no transfers are active
    if (!impl_.hasActiveTransfers()) {
        impl_.startDownload(idx);
    }

    util::logLine("download_ui: added game " + game.title + " (topic_id=" + game.topic_id + ") to download queue");
    saveDownloads();
}

bool DownloadManager::pauseDownload(const std::string& topic_id) {
    auto& queue = const_cast<std::vector<download::DownloadItem>&>(impl_.queue());
    for (size_t i = 0; i < queue.size(); ++i) {
        auto& item = queue[i];
        if (item.topic_id == topic_id) {
            if (item.state == download::DownloadState::Downloading ||
                item.state == download::DownloadState::StreamPreparing ||
                item.state == download::DownloadState::StreamInstalling) {

                if (!item.torrent_hash.empty()) {
                    tsnx_engine_pause_torrent(nullptr, item.torrent_hash.c_str());
                }

                item.state = download::DownloadState::Paused;
                item.download_speed_kbps = 0.0f;
                util::logLine("download_ui: paused topic_id=" + topic_id);
                saveDownloads();
                triggerCallback();
                return true;
            }
        }
    }
    return false;
}

bool DownloadManager::resumeDownload(const std::string& topic_id) {
    auto& queue = const_cast<std::vector<download::DownloadItem>&>(impl_.queue());
    for (size_t i = 0; i < queue.size(); ++i) {
        auto& item = queue[i];
        if (item.topic_id == topic_id) {
            if (item.state == download::DownloadState::Paused || item.state == download::DownloadState::Installing) {
                if (!item.torrent_hash.empty()) {
                    tsnx_engine_resume_torrent(nullptr, item.torrent_hash.c_str());
                }
                item.state = download::DownloadState::Downloading;
                util::logLine("download_ui: resumed topic_id=" + topic_id);
                saveDownloads();
                triggerCallback();
                return true;
            }
        }
    }
    return false;
}

bool DownloadManager::cancelDownload(const std::string& topic_id) {
    auto& queue = const_cast<std::vector<download::DownloadItem>&>(impl_.queue());
    for (size_t i = 0; i < queue.size(); ++i) {
        auto& item = queue[i];
        if (item.topic_id == topic_id) {
            impl_.cancelDownload(i);
            util::logLine("download_ui: cancelled topic_id=" + topic_id);
            saveDownloads();
            triggerCallback();
            return true;
        }
    }
    return false;
}

bool DownloadManager::deleteDownload(const std::string& topic_id) {
    auto& queue = const_cast<std::vector<download::DownloadItem>&>(impl_.queue());
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].topic_id == topic_id) {
            impl_.cancelDownload(i);
            util::logLine("download_ui: deleted topic_id=" + topic_id + " from queue");
            queue.erase(queue.begin() + i);
            saveDownloads();
            triggerCallback();
            return true;
        }
    }
    return false;
}

int DownloadManager::getActiveDownloadsCount() const {
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
    // Clean up old downloads.json file if present
    std::remove("sdmc:/switch/TorrentShopNX/downloads.json");
}

} // namespace ui
