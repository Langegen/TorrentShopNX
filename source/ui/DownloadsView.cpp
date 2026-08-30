#include "DownloadsView.hpp"
#include "DownloadUiManager.hpp"
#include "../config/config.h"
#include "../utils/switch_utils.h"
#include <iomanip>
#include <chrono>
#include <cmath>

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
    lastInputTime_ = std::chrono::steady_clock::now();
}

void DownloadsView::onContentAvailable() {

    recycler->registerCell("Download", []() { return DownloadCell::create(); });
    recycler->setDataSource(new DownloadsDataSource(this));

    // Register hidden button action to toggle screen backlight (shown in top-right header instead of footer)
    this->registerAction("", brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        toggleBacklight();
        return true;
    }, true /* hidden from footer */);

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_MINUS, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        toggleBacklight();
        return true;
    });

    // Start repeating timer for auto-sleep / backlight timeout monitoring
    backlightTimer_ = new brls::RepeatingTimer();
    backlightTimer_->setPeriod(200);
    backlightTimer_->setCallback([this]() {
        checkBacklightState();
    });
    backlightTimer_->start();

    // Register callback for auto-refreshing the view when progress updates
    ui::DownloadManager::instance().setProgressCallback([this]() {
        brls::sync([this]() {
            const auto& queue = ui::DownloadManager::instance().getImpl().queue();
            
            if (queue.empty()) {
                emptyLabel->setVisibility(brls::Visibility::VISIBLE);
                recycler->setVisibility(brls::Visibility::GONE);
            } else {
                emptyLabel->setVisibility(brls::Visibility::GONE);
                recycler->setVisibility(brls::Visibility::VISIBLE);
            }

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
    if (backlightTimer_) {
        backlightTimer_->stop();
        delete backlightTimer_;
        backlightTimer_ = nullptr;
    }
    if (util::isBacklightOff()) {
        util::setBacklightOff(false);
    }
    // Unregister callback on destruction to avoid crashes
    ui::DownloadManager::instance().setProgressCallback(nullptr);
}

void DownloadsView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    lastInputTime_ = std::chrono::steady_clock::now();
    if (!ui::DownloadManager::instance().getImpl().queue().empty()) {
        brls::Application::giveFocus(recycler);
    }
}

void DownloadsView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    if (util::isBacklightOff()) {
        util::setBacklightOff(false);
    }
    ui::DownloadManager::instance().setProgressCallback(nullptr);
}

void DownloadsView::toggleBacklight() {
    bool isOff = util::isBacklightOff();
    util::setBacklightOff(!isOff);
    auto now = std::chrono::steady_clock::now();
    lastInputTime_ = now;
    backlightToggleTime_ = now;
}

void DownloadsView::checkBacklightState() {
    const auto now = std::chrono::steady_clock::now();
    const auto& cState = brls::Application::getControllerState();

    if (isFirstStateCheck_) {
        prevControllerState_ = cState;
        isFirstStateCheck_ = false;
        return;
    }

    // Check for NEW input (button pressed down on this frame, or stick moved)
    bool hasNewButtonPress = false;
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (cState.buttons[i] && !prevControllerState_.buttons[i]) {
            hasNewButtonPress = true;
            break;
        }
    }

    bool hasStickMoved = false;
    if (std::abs(cState.axes[brls::LEFT_X]) > 0.6f ||
        std::abs(cState.axes[brls::LEFT_Y]) > 0.6f ||
        std::abs(cState.axes[brls::RIGHT_X]) > 0.6f ||
        std::abs(cState.axes[brls::RIGHT_Y]) > 0.6f) {
        hasStickMoved = true;
    }

    bool hasAnyHeldButton = false;
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (cState.buttons[i]) {
            hasAnyHeldButton = true;
            break;
        }
    }

    // If user is actively pressing buttons or moving sticks, update activity timestamp
    if (hasNewButtonPress || hasStickMoved || hasAnyHeldButton) {
        lastInputTime_ = now;
    }

    // If backlight is currently OFF:
    if (util::isBacklightOff()) {
        int activeCount = ui::DownloadManager::instance().getActiveDownloadsCount();

        // If all downloads have completed, turn backlight back on to notify user
        if (activeCount == 0) {
            util::setBacklightOff(false);
            prevControllerState_ = cState;
            return;
        }

        // Debounce: ignore inputs during the first 800ms after toggling to avoid immediate re-wake
        auto msSinceToggle = std::chrono::duration_cast<std::chrono::milliseconds>(now - backlightToggleTime_).count();
        if (msSinceToggle >= 800) {
            // Wake up on NEW button press or stick movement
            if (hasNewButtonPress || hasStickMoved) {
                util::setBacklightOff(false);
                lastInputTime_ = now;
            }
        }
        prevControllerState_ = cState;
        return;
    }

    // If backlight is currently ON: check auto-dim timeout
    int activeCount = ui::DownloadManager::instance().getActiveDownloadsCount();
    int timeoutSec = config::ConfigManager::instance().getBacklightTimeout();
    if (timeoutSec > 0 && activeCount > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastInputTime_).count();
        if (elapsed >= timeoutSec) {
            util::setBacklightOff(true);
            backlightToggleTime_ = now;
        }
    }

    prevControllerState_ = cState;
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
        return brls::getStr("app/downloads/eta_h_m", std::to_string(hr), std::to_string(min));
    } else if (sec > 60.0) {
        int min = (int)(sec / 60.0);
        int s = (int)(sec - min * 60.0);
        return brls::getStr("app/downloads/eta_m_s", std::to_string(min), std::to_string(s));
    } else {
        return brls::getStr("app/downloads/eta_s", std::to_string((int)sec));
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

static std::string formatElapsed(std::chrono::steady_clock::time_point start) {
    if (start.time_since_epoch().count() == 0) return "--:--";
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
    if (secs < 0) secs = 0;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    char buf[24];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    }
    return std::string(buf);
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
    std::string statusStr = "app/downloads/state_waiting"_i18n;
    NVGcolor statusColor = nvgRGB(200, 200, 200); // Gray
    
    switch (item.state) {
        case download::DownloadState::Queued:
            statusStr = "app/downloads/state_queued"_i18n;
            statusColor = nvgRGB(255, 193, 7); // Amber
            break;
        case download::DownloadState::Downloading:
            statusStr = "app/downloads/state_downloading"_i18n;
            statusColor = nvgRGB(76, 175, 80); // Green
            break;
        case download::DownloadState::StreamPreparing:
            statusStr = "app/downloads/state_preparing"_i18n;
            statusColor = nvgRGB(0, 188, 212); // Cyan
            break;
        case download::DownloadState::StreamInstalling:
        case download::DownloadState::Installing:
            if (item.peers <= 0 && item.download_speed_kbps <= 0.0) {
                statusStr = "app/downloads/state_no_peers"_i18n;
                statusColor = nvgRGB(255, 152, 0); // Orange
            } else {
                statusStr = "app/downloads/state_installing"_i18n;
                statusColor = nvgRGB(33, 150, 243); // Blue
            }
            break;
        case download::DownloadState::Completed:
            statusStr = "app/downloads/state_completed"_i18n;
            statusColor = nvgRGB(139, 195, 74); // Light Green
            break;
        case download::DownloadState::Cancelled:
            statusStr = "app/downloads/state_cancelled"_i18n;
            statusColor = nvgRGB(244, 67, 54); // Red
            break;
        case download::DownloadState::Failed:
            statusStr = "app/downloads/state_failed"_i18n;
            statusColor = nvgRGB(244, 67, 54); // Red
            break;
        case download::DownloadState::Paused:
            statusStr = "app/downloads/state_paused"_i18n;
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
        // Не-игровые файлы сохраняются в downloads/ по реальному пути; игры
        // устанавливаются в систему, поэтому путь downloads/ им не показываем.
        if (!item.file_dl_dest.empty()) {
            cell->statsText->setText(brls::getStr("app/downloads/saved_to", item.file_dl_dest));
        } else {
            cell->statsText->setText("app/downloads/installed"_i18n);
        }
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else if (item.state == download::DownloadState::Failed) {
        cell->statsText->setText(item.error_message.empty() ? "app/downloads/unknown_error"_i18n : item.error_message);
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->statsText->setVisibility(brls::Visibility::GONE);
    }

    // Таймер затраченного времени: показываем во время скачивания/установки
    // и после завершения (общее время).
    const bool showsElapsed =
        item.state == download::DownloadState::Downloading ||
        item.state == download::DownloadState::StreamPreparing ||
        item.state == download::DownloadState::StreamInstalling ||
        item.state == download::DownloadState::Installing ||
        item.state == download::DownloadState::Completed;
    if (showsElapsed) {
        cell->elapsedText->setText(brls::getStr("app/downloads/elapsed", formatElapsed(item.start_time)));
        cell->elapsedText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->elapsedText->setVisibility(brls::Visibility::GONE);
    }

    // Seeders and Peers count text
    if (item.state == download::DownloadState::Downloading || 
        item.state == download::DownloadState::StreamInstalling ||
        item.state == download::DownloadState::StreamPreparing) {
        
        std::string peersStr;
        if (item.known_peers > item.peers) {
            peersStr = brls::getStr("app/downloads/peers_with_known", std::to_string(item.seeds), std::to_string(item.peers), std::to_string(item.known_peers), std::to_string(item.dht));
        } else {
            peersStr = brls::getStr("app/downloads/peers_standard", std::to_string(item.seeds), std::to_string(item.peers), std::to_string(item.dht));
        }
        cell->peersText->setText(peersStr);
        cell->peersText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->peersText->setVisibility(brls::Visibility::GONE);
    }

    // Configure Gamepad Controls on Cell Focus
    if (item.state == download::DownloadState::Completed || 
        item.state == download::DownloadState::Cancelled || 
        item.state == download::DownloadState::Failed) {
        
        cell->registerAction("app/downloads/action_delete"_i18n, brls::ControllerButton::BUTTON_Y, [topic_id = item.topic_id](brls::View* view) {
            ui::DownloadManager::instance().deleteDownload(topic_id);
            return true;
        });
    } else {
        cell->registerAction(item.state == download::DownloadState::Paused ? "app/downloads/action_resume"_i18n : "app/downloads/action_pause"_i18n, 
                             brls::ControllerButton::BUTTON_A, [topic_id = item.topic_id, state = item.state](brls::View* view) {
            if (state == download::DownloadState::Paused) {
                ui::DownloadManager::instance().resumeDownload(topic_id);
            } else {
                ui::DownloadManager::instance().pauseDownload(topic_id);
            }
            return true;
        });

        cell->registerAction("app/downloads/action_cancel"_i18n, brls::ControllerButton::BUTTON_X, [topic_id = item.topic_id](brls::View* view) {
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
