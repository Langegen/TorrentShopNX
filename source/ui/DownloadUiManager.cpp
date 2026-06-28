#include "DownloadUiManager.hpp"
#include <fstream>
#include <algorithm>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"
#include "../torrent/torrent_engine.h"
#include "../config/config.h"

namespace ui {

void DownloadManager::init() {
    auto& cfg = config::ConfigManager::instance();
    impl_.dataSourceManager().setRemoteUrl(cfg.getTorrServerUrl());
    if (cfg.getDataMode() == "local_client") {
        impl_.dataSourceManager().setMode(datasource::DataSourceMode::LocalClient);
    } else {
        impl_.dataSourceManager().setMode(datasource::DataSourceMode::Remote);
    }
    
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
                
                if (item.torrent_id >= 0) {
                    // For TorrServer
                    impl_.dataSourceManager().getSource(); // Ensure torrent engine gets initialized
                    // We can pause via API
                    // In torrent_manager: pauseTorrent(id)
                    auto torrent = impl_.dataSourceManager().getSource();
                    if (item.torrent_id >= 0 && impl_.hasActiveTransfers()) {
                        // Pause the torrent using our wrapper or engine
                        torrent::TorrentEngine::instance().pauseTorrent(item.torrent_hash);
                    }
                } else if (!item.torrent_hash.empty()) {
                    torrent::TorrentEngine::instance().pauseTorrent(item.torrent_hash);
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
                if (item.torrent_id >= 0) {
                    // Resume TorrServer
                    // TorrServer uses resume
                    torrent::TorrentEngine::instance().resumeTorrent(item.torrent_hash);
                } else if (!item.torrent_hash.empty()) {
                    torrent::TorrentEngine::instance().resumeTorrent(item.torrent_hash);
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
    auto it = std::remove_if(queue.begin(), queue.end(), [&topic_id, this](const download::DownloadItem& item) {
        if (item.topic_id == topic_id) {
            // Cancel if active
            if (item.state != download::DownloadState::Completed && 
                item.state != download::DownloadState::Cancelled && 
                item.state != download::DownloadState::Failed) {
                // Should cancel first
            }
            util::logLine("download_ui: deleted topic_id=" + topic_id + " from queue");
            return true;
        }
        return false;
    });
    
    if (it != queue.end()) {
        queue.erase(it, queue.end());
        saveDownloads();
        triggerCallback();
        return true;
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
