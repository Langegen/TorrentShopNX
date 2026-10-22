#include "DownloadUiManager.hpp"
#include <fstream>
#include <algorithm>
#include <borealis.hpp>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"
#include "../utils/app_paths.h"
#include "../utils/string_utils.h"
#include "../config/config.h"

#include <engine/engine.h>
#include "../net/image_downloader.h"

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

    net::ImageDownloader::instance().setActiveCheckFn([this]() {
        return getActiveDownloadsCount() > 0;
    });

    // Load and restore previous downloads
    loadDownloads();
}

void DownloadManager::shutdown() {
    impl_.shutdown();
}

bool DownloadManager::hasActiveDownload(const std::string& topic_id, const std::string& magnet, int forced_file_index) const {
    std::lock_guard<std::recursive_mutex> lock(const_cast<download::DownloadManager&>(impl_).queueMutex());
    std::string newHash;
    if (!magnet.empty()) {
        newHash = util::extractBtihHash(magnet);
    }

    for (const auto& item : impl_.queue()) {
        if (item.state == download::DownloadState::Completed ||
            item.state == download::DownloadState::Cancelled ||
            item.state == download::DownloadState::Failed) {
            continue;
        }

        // 1. Direct topic_id match (if non-empty)
        if (!topic_id.empty() && !item.topic_id.empty() && item.topic_id == topic_id) {
            return true;
        }

        // 2. Hash and file index match
        if (!newHash.empty()) {
            std::string itemHash = item.torrent_hash;
            if (itemHash.empty() && !item.magnet.empty()) {
                itemHash = util::extractBtihHash(item.magnet);
            }
            if (!itemHash.empty() && util::equalsIgnoreCase(newHash, itemHash)) {
                // If either is full torrent (forced_file_index == -1), or both target same file
                if (forced_file_index == -1 || item.forced_file_index == -1 || item.forced_file_index == forced_file_index) {
                    return true;
                }
            }
        }
    }
    return false;
}

void DownloadManager::addDownload(const Game& game, const std::vector<int>& selected_files, int forced_file_index, const std::string& forced_stream_name, const std::string& retro_console_id,
                                 const std::string& group_id, const std::string& group_title, const std::string& component_label, const std::string& component_name, int component_type,
                                 unsigned long long initial_file_size, const std::string& retro_target_mode) {
    if (hasActiveDownload(game.topic_id, game.magnet, forced_file_index)) {
        util::logLine("download_ui: game " + game.title + " (topic_id=" + game.topic_id + ") is already in download queue, ignoring duplicate");
        brls::Application::notify("app/downloads/already_in_queue"_i18n);
        return;
    }

    size_t idx = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
        idx = impl_.addToQueue(game.title, game.magnet, forced_file_index, forced_stream_name, isHomebrewGame(game), retro_console_id, retro_target_mode);

        auto& item = impl_.queueMutable()[idx];
        item.topic_id = game.topic_id;
        item.title_id = game.title_id;
        item.selected_files = selected_files;
        item.priorities_set = false;
        item.cover_url = game.cover;
        item.is_homebrew = isHomebrewGame(game);
        item.retro_console_id = retro_console_id;
        item.retro_target_mode = retro_target_mode;

        item.download_total_bytes = initial_file_size;
        item.install_total = initial_file_size;

        item.group_id = !group_id.empty() ? group_id : game.topic_id;
        item.group_title = !group_title.empty() ? group_title : cleanTitle(game.title);
        item.component_label = component_label;
        item.component_name = component_name;
        item.component_type = component_type;

        // Start the download immediately if no transfers are active
        if (!impl_.hasActiveTransfers()) {
            impl_.startDownload(idx);
        }

        util::logLine("download_ui: added game " + game.title + " (topic_id=" + game.topic_id + ") to download queue, is_homebrew=" + (item.is_homebrew ? "true" : "false") + ", retro=" + retro_console_id + ", group=" + item.group_id + ", size=" + std::to_string(initial_file_size));
    }
    saveDownloads();
}

void DownloadManager::addPortInstall(const Game& game, std::shared_ptr<installer::PortInstallPlan> plan, int variant_index) {
    if (hasActiveDownload(game.topic_id, game.magnet, -1)) {
        util::logLine("download_ui: port install already in download queue for " + game.title + " (topic_id=" + game.topic_id + "), ignoring duplicate");
        brls::Application::notify("app/downloads/already_in_queue"_i18n);
        return;
    }

    size_t idx = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_.queueMutex());
        idx = impl_.addPortInstall(game.title, game.magnet, plan, variant_index);

        auto& item = impl_.queueMutable()[idx];
        item.topic_id = game.topic_id;
        item.title_id = game.title_id;
        item.cover_url = game.cover;
        item.group_id = game.topic_id;
        item.group_title = cleanTitle(game.title);
        item.component_label = "OTH";
        item.component_name = cleanTitle(game.title);
        item.component_type = 4;

        if (plan && variant_index >= 0 && (size_t)variant_index < (size_t)plan->variants.size()) {
            item.download_total_bytes = plan->variants[variant_index].total_size + plan->forwarder_nsp_size;
            item.install_total = item.download_total_bytes;
        }

        // Start the download immediately if no transfers are active
        if (!impl_.hasActiveTransfers()) {
            impl_.startDownload(idx);
        }

        util::logLine("download_ui: added port auto install for " + game.title + " (topic_id=" + game.topic_id + ") variant=" + std::to_string(variant_index) + " size=" + std::to_string(item.download_total_bytes));
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
                if (!impl_.hasActiveTransfers()) {
                    impl_.startNextDownload();
                }
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
