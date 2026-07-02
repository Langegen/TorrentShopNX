#include "DownloadsView.hpp"
#include "DownloadUiManager.hpp"
#include <iomanip>

extern std::vector<Game> g_games;

namespace ui {

// DOWNLOADCELL IMPLEMENTATION
DownloadCell::DownloadCell() {
    this->inflateFromXMLRes("xml/download_cell.xml");
}

DownloadCell* DownloadCell::create() {
    return new DownloadCell();
}

DownloadCell::~DownloadCell() {
    if (imageToken) *imageToken = false;
}

// DOWNLOADSVIEW IMPLEMENTATION
DownloadsView::DownloadsView() {
}

void DownloadsView::onContentAvailable() {

    recycler->registerCell("Download", []() { return DownloadCell::create(); });
    recycler->setDataSource(new DownloadsDataSource(this));

    // Register callback for auto-refreshing the view when progress updates
    ui::DownloadManager::instance().setProgressCallback([this]() {
        brls::sync([this]() {
            const auto& queue = ui::DownloadManager::instance().getImpl().queue();
            size_t currentRows = queue.size();
            if (currentRows != lastRows_) {
                lastRows_ = currentRows;
                recycler->reloadData();
                return;
            }

            // Update visible cells in-place to avoid focus jumping
            brls::Box* contentBox = nullptr;
            for (auto* child : recycler->getChildren()) {
                brls::Box* box = dynamic_cast<brls::Box*>(child);
                if (box) {
                    contentBox = box;
                    break;
                }
            }
            if (contentBox) {
                for (auto* cellView : contentBox->getChildren()) {
                    DownloadCell* cell = dynamic_cast<DownloadCell*>(cellView);
                    if (cell) {
                        int row = cell->getIndexPath().row;
                        if (row >= 0 && static_cast<size_t>(row) < queue.size()) {
                            updateCell(cell, queue[row]);
                        }
                    }
                }
            }
        });
    });

    // Force an immediate refresh so that cells show the current download state
    // right away after the auto-redirect from FileSelectView (without waiting for
    // the next progress-thread tick which can be up to 1 second away).
    ui::DownloadManager::instance().triggerCallback();
}

DownloadsView::~DownloadsView() {
    // Unregister callback on destruction to avoid crashes
    ui::DownloadManager::instance().setProgressCallback(nullptr);
}



static std::string formatKbps(float speed_kbps, bool is_download) {
    std::string prefix = is_download ? "↓ " : "↑ ";
    if (speed_kbps >= 1024.0f) {
        float mbps = speed_kbps / 1024.0f;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.2f MB/s", prefix.c_str(), mbps);
        return std::string(buf);
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.1f KB/s", prefix.c_str(), speed_kbps);
        return std::string(buf);
    }
}

static std::string formatEta(float speed_kbps, unsigned long long remaining_bytes) {
    if (speed_kbps <= 5.0f || remaining_bytes == 0) {
        return "ETA: --";
    }
    double speed_bytes = (double)speed_kbps * 1024.0;
    double sec = (double)remaining_bytes / speed_bytes;
    if (sec > 3600.0) {
        int hr = (int)(sec / 3600.0);
        int min = (int)((sec - hr * 3600.0) / 60.0);
        return "ETA: " + std::to_string(hr) + "ч " + std::to_string(min) + "м";
    } else if (sec > 60.0) {
        int min = (int)(sec / 60.0);
        int s = (int)(sec - min * 60.0);
        return "ETA: " + std::to_string(min) + "м " + std::to_string(s) + "с";
    } else {
        return "ETA: " + std::to_string((int)sec) + "с";
    }
}

static std::string formatBytes(unsigned long long bytes) {
    double val = (double)bytes / 1024.0 / 1024.0; // in MB
    if (val >= 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f GB", val / 1024.0);
        return std::string(buf);
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", val);
        return std::string(buf);
    }
}

static std::string formatProgressBytes(unsigned long long written, unsigned long long total) {
    return formatBytes(written) + " / " + formatBytes(total);
}

// DATASOURCE IMPLEMENTATION
int DownloadsView::DownloadsDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return ui::DownloadManager::instance().getImpl().queue().size();
}

void DownloadsView::updateCell(DownloadCell* cell, const download::DownloadItem& item) {
    cell->title->setText(cleanTitle(item.title));
    
    // Find game cover from global catalog list (strip index suffix from topic_id if present)
    std::string origTopicId = item.topic_id;
    size_t underscorePos = origTopicId.find('_');
    if (underscorePos != std::string::npos) {
        origTopicId = origTopicId.substr(0, underscorePos);
    }

    std::string coverUrl;
    for (const auto& g : g_games) {
        if (g.topic_id == origTopicId) {
            coverUrl = g.cover;
            break;
        }
    }
    if (!coverUrl.empty()) {
        setImageFromHTTPS(cell->cover, coverUrl, cell->imageToken);
    } else {
        cell->cover->setImageFromFile("romfs:/img/borealis_96.png"); // fallback
    }

    // Set download progress
    float progress = item.progress;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    cell->progressBar->setWidth(progress * 300.0f);
    
    char progressBuf[16];
    std::snprintf(progressBuf, sizeof(progressBuf), "%d%%", (int)(progress * 100.0f));
    std::string progressStr = progressBuf;

    unsigned long long dl_written = 0;
    unsigned long long dl_total = 0;
    if (item.hybrid_installer) {
        dl_written = item.hybrid_installer->bytesDownloaded();
        dl_total = item.hybrid_installer->downloadTotalBytes();
    } else {
        dl_total = item.install_total;
        dl_written = (dl_total > 0) ? (unsigned long long)((double)dl_total * progress) : 0;
    }

    if (item.state == download::DownloadState::Downloading || 
        item.state == download::DownloadState::StreamInstalling) {
        progressStr += "  (" + formatKbps(item.download_speed_kbps, true) + ", " + formatProgressBytes(dl_written, dl_total) + ")";
    } else {
        progressStr += "  (" + formatProgressBytes(dl_written, dl_total) + ")";
    }
    cell->progressText->setText(progressStr);

    // Manage installation progress bar row visibility
    const bool showsInstall = (item.state == download::DownloadState::StreamPreparing || 
                               item.state == download::DownloadState::StreamInstalling || 
                               item.state == download::DownloadState::Installing ||
                               item.state == download::DownloadState::Completed);
    
    if (showsInstall) {
        cell->installProgressRow->setVisibility(brls::Visibility::VISIBLE);
        float instProgress = item.install_progress;
        if (instProgress < 0.0f) instProgress = 0.0f;
        if (instProgress > 1.0f) instProgress = 1.0f;
        cell->installBar->setWidth(instProgress * 300.0f);
        
        char instBuf[16];
        std::snprintf(instBuf, sizeof(instBuf), "%d%%", (int)(instProgress * 100.0f));
        std::string instStr = instBuf;

        unsigned long long inst_written = item.install_written;
        unsigned long long inst_total = item.install_total;
        if (inst_total == 0 && item.hybrid_installer) {
            inst_total = item.hybrid_installer->totalBytes();
        }

        if (item.state == download::DownloadState::StreamInstalling ||
            item.state == download::DownloadState::Installing) {
            instStr += "  (" + formatKbps(item.install_speed_kbps, false) + ", " + formatProgressBytes(inst_written, inst_total) + ")";
        } else {
            instStr += "  (" + formatProgressBytes(inst_written, inst_total) + ")";
        }
        cell->installText->setText(instStr);
    } else {
        cell->installProgressRow->setVisibility(brls::Visibility::GONE);
    }

    // Status descriptions mapping
    std::string statusStr = "Ожидание";
    NVGcolor statusColor = nvgRGB(200, 200, 200); // Gray
    
    switch (item.state) {
        case download::DownloadState::Queued:
            statusStr = "В очереди";
            statusColor = nvgRGB(255, 193, 7); // Amber
            break;
        case download::DownloadState::Downloading:
            statusStr = "Загрузка";
            statusColor = nvgRGB(76, 175, 80); // Green
            break;
        case download::DownloadState::StreamPreparing:
            statusStr = "Ожидание метаданных";
            statusColor = nvgRGB(0, 188, 212); // Cyan
            break;
        case download::DownloadState::StreamInstalling:
        case download::DownloadState::Installing:
            statusStr = "Установка";
            statusColor = nvgRGB(33, 150, 243); // Blue
            break;
        case download::DownloadState::Completed:
            statusStr = "Завершено ✓";
            statusColor = nvgRGB(139, 195, 74); // Light Green
            break;
        case download::DownloadState::Cancelled:
            statusStr = "Отменено";
            statusColor = nvgRGB(244, 67, 54); // Red
            break;
        case download::DownloadState::Failed:
            statusStr = "Ошибка";
            statusColor = nvgRGB(244, 67, 54); // Red
            break;
        case download::DownloadState::Paused:
            statusStr = "Пауза";
            statusColor = nvgRGB(255, 152, 0); // Orange
            break;
    }
    
    cell->statusText->setText(statusStr);
    cell->statusText->setTextColor(statusColor);

    // Stats text
    if (item.state == download::DownloadState::Downloading || 
        item.state == download::DownloadState::StreamInstalling) {
        
        // Calculate remaining download bytes
        unsigned long long remaining_dl = (dl_total >= dl_written) ? (dl_total - dl_written) : 0;
        
        std::string stats = formatEta(item.download_speed_kbps, remaining_dl);
        cell->statsText->setText(stats);
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else if (item.state == download::DownloadState::Completed) {
        cell->statsText->setText("Сохранено в: /switch/TorrentShopNX/downloads/" + item.topic_id + "/");
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else if (item.state == download::DownloadState::Failed) {
        cell->statsText->setText(item.error_message.empty() ? "Неизвестная ошибка" : item.error_message);
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->statsText->setVisibility(brls::Visibility::GONE);
    }

    // Seeders and Peers count text
    if (item.state == download::DownloadState::Downloading || 
        item.state == download::DownloadState::StreamInstalling ||
        item.state == download::DownloadState::StreamPreparing) {
        
        char peersBuf[128];
        std::snprintf(peersBuf, sizeof(peersBuf), "Сиды: %d · Пиры: %d · DHT: %d", item.seeds, item.peers, item.dht);
        cell->peersText->setText(peersBuf);
        cell->peersText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->peersText->setVisibility(brls::Visibility::GONE);
    }

    // Configure Gamepad Controls on Cell Focus
    if (item.state == download::DownloadState::Completed || 
        item.state == download::DownloadState::Cancelled || 
        item.state == download::DownloadState::Failed) {
        
        cell->registerAction("Удалить из списка", brls::ControllerButton::BUTTON_Y, [topic_id = item.topic_id](brls::View* view) {
            ui::DownloadManager::instance().deleteDownload(topic_id);
            return true;
        });
    } else {
        cell->registerAction(item.state == download::DownloadState::Paused ? "Возобновить" : "Приостановить", 
                             brls::ControllerButton::BUTTON_A, [topic_id = item.topic_id, state = item.state](brls::View* view) {
            if (state == download::DownloadState::Paused) {
                ui::DownloadManager::instance().resumeDownload(topic_id);
            } else {
                ui::DownloadManager::instance().pauseDownload(topic_id);
            }
            return true;
        });

        cell->registerAction("Отменить загрузку", brls::ControllerButton::BUTTON_X, [topic_id = item.topic_id](brls::View* view) {
            ui::DownloadManager::instance().cancelDownload(topic_id);
            return true;
        });
    }
}


brls::RecyclerCell* DownloadsView::DownloadsDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    DownloadCell* cell = dynamic_cast<DownloadCell*>(recycler->dequeueReusableCell("Download"));
    if (!cell) return nullptr;

    int row = index.row;
    
    if (cell->imageToken) *(cell->imageToken) = false;
    cell->imageToken = std::make_shared<bool>(true);

    const auto& queue = ui::DownloadManager::instance().getImpl().queue();
    if (static_cast<size_t>(row) < queue.size()) {
        parent_->updateCell(cell, queue[row]);
    }

    return cell;
}

void DownloadsView::DownloadsDataSource::didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    // Standard click is mapped to controller button actions (A button handles toggle pause/resume automatically)
}

} // namespace ui
