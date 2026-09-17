#include "DownloadsView.hpp"
#include "DownloadUiManager.hpp"
#include "FileManagerView.hpp"
#include "../config/config.h"
#include "../utils/switch_utils.h"
#include "../utils/app_paths.h"
#include "../utils/file_ops.h"
#include <engine/engine.h>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <filesystem>

extern std::vector<Game> g_games;

namespace ui {

namespace {

bool isFileDownloadItem(const download::DownloadItem& item) {
    if (item.is_homebrew || !item.retro_console_id.empty() || item.file_dl_dispatched || !item.file_dl_dest.empty()) {
        return true;
    }
    if (!item.forced_stream_name.empty() && !util::isGamePackage(item.forced_stream_name)) {
        return true;
    }
    if (!item.hybrid_installer && item.state == download::DownloadState::Completed) {
        return true;
    }
    return false;
}

void openDownloadsFolderForItem(const download::DownloadItem& item) {
    std::string targetDir = TSNX_DOWNLOADS_DIR;
    std::string focusChild;

    if (!item.file_dl_dest.empty()) {
        std::filesystem::path p(item.file_dl_dest);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            targetDir = p.generic_string();
        } else {
            std::filesystem::path parent = p.parent_path();
            if (std::filesystem::exists(parent, ec) && std::filesystem::is_directory(parent, ec)) {
                targetDir = parent.generic_string();
                focusChild = p.filename().generic_string();
            }
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);

    brls::Application::pushActivity(new ui::FileManagerView(targetDir, focusChild));
}

} // namespace

static void showPeerInspector(const download::DownloadItem& item);

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

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_I, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
        const auto& queue = ui::DownloadManager::instance().getImpl().queue();
        if (focusedRow_ >= 0 && static_cast<size_t>(focusedRow_) < queue.size()) {
            showPeerInspector(queue[focusedRow_]);
        }
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
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
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
                if (!queue.empty()) {
                    int validRow = std::clamp(focusedRow_, 0, static_cast<int>(queue.size()) - 1);
                    recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                    recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                    brls::Application::giveFocus(recycler);
                }
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

            // If focus was lost and queue is not empty, restore focus
            if (!queue.empty() && brls::Application::getCurrentFocus() == nullptr) {
                int validRow = std::clamp(focusedRow_, 0, static_cast<int>(queue.size()) - 1);
                recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                brls::Application::giveFocus(recycler);
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
    std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
    const auto& queue = ui::DownloadManager::instance().getImpl().queue();
    if (!queue.empty()) {
        int validRow = std::clamp(focusedRow_, 0, static_cast<int>(queue.size()) - 1);
        recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
        recycler->selectRowAt(brls::IndexPath(0, validRow), false);
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
    const int axis_indices[] = { brls::LEFT_X, brls::LEFT_Y, brls::RIGHT_X, brls::RIGHT_Y };
    for (int ax : axis_indices) {
        float delta = std::abs(cState.axes[ax] - prevControllerState_.axes[ax]);
        if (delta > 0.25f && std::abs(cState.axes[ax]) > 0.35f) {
            hasStickMoved = true;
            break;
        }
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

static void showPeerInspector(const download::DownloadItem& item) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(680.0f);
    content->setPadding(20.0f);

    // Header: Title
    auto* headerTitle = new brls::Label();
    headerTitle->setText("app/downloads/peers_title"_i18n);
    headerTitle->setFontSize(22.0f);
    headerTitle->setTextColor(nvgRGB(255, 255, 255));
    headerTitle->setMarginBottom(4.0f);
    content->addView(headerTitle);

    auto* subTitle = new brls::Label();
    subTitle->setText(cleanTitle(item.title));
    subTitle->setFontSize(14.0f);
    subTitle->setTextColor(nvgRGB(180, 180, 180));
    subTitle->setSingleLine(true);
    subTitle->setMarginBottom(16.0f);
    content->addView(subTitle);

    std::string hash = item.torrent_hash;
    if (hash.empty()) {
        hash = item.topic_id;
    }

    tsnx_engine_diag diag{};
    bool hasDiag = false;
    if (!hash.empty()) {
        hasDiag = tsnx_engine_get_diag(nullptr, hash.c_str(), &diag);
    }

    // Diagnostics stats box
    auto* statsBox = new brls::Box();
    statsBox->setAxis(brls::Axis::COLUMN);
    statsBox->setBackgroundColor(nvgRGBA(36, 39, 46, 180));
    statsBox->setCornerRadius(8.0f);
    statsBox->setPadding(12.0f);
    statsBox->setMarginBottom(16.0f);

    auto addStatRow = [statsBox](const std::string& label, const std::string& val) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setMarginBottom(4.0f);

        auto* l = new brls::Label();
        l->setText(label);
        l->setFontSize(14.0f);
        l->setTextColor(nvgRGB(180, 180, 180));

        auto* v = new brls::Label();
        v->setText(val);
        v->setFontSize(14.0f);
        v->setTextColor(nvgRGB(255, 255, 255));

        row->addView(l);
        row->addView(v);
        statsBox->addView(row);
    };

    if (hasDiag) {
        std::string peersStr = std::to_string(diag.live) + " live (" + std::to_string(diag.connecting) + " connecting, " + std::to_string(diag.peak) + " peak)";
        addStatRow("Peers / Connections", peersStr);

        std::string workStr = std::to_string(diag.claiming) + " downloading, " + std::to_string(diag.idle) + " idle / choked";
        addStatRow("Session Activity", workStr);

        if (diag.pieces_total > 0) {
            std::string pieceStr = std::to_string(diag.pieces_done) + " / " + std::to_string(diag.pieces_total) +
                                   " (" + std::to_string((int)((diag.pieces_done * 100) / diag.pieces_total)) + "%)";
            addStatRow("Pieces Resident", pieceStr);
        }

        std::string dataStr = formatBytes(diag.bytes_recv);
        if (diag.dup_bytes > 0) {
            dataStr += " (dup " + formatBytes(diag.dup_bytes) + ")";
        }
        addStatRow("Data Received", dataStr);

        std::string errorsStr = "Timeouts: " + std::to_string(diag.timeouts) + " · Sock: " + std::to_string(diag.sock_fail) + " · Handshake: " + std::to_string(diag.hs_fail);
        addStatRow("Connection Issues", errorsStr);
    } else {
        std::string seedsPeers = "Seeds: " + std::to_string(item.seeds) + " · Peers: " + std::to_string(item.peers) + " · DHT: " + std::to_string(item.dht);
        addStatRow("Swarm Summary", seedsPeers);
        if (!hash.empty()) {
            addStatRow("Info Hash", hash);
        }
    }
    content->addView(statsBox);

    // Peer List Header
    auto* peerListHeader = new brls::Label();
    peerListHeader->setText("Active Peer Sessions");
    peerListHeader->setFontSize(15.0f);
    peerListHeader->setTextColor(nvgRGB(200, 200, 200));
    peerListHeader->setMarginBottom(8.0f);
    content->addView(peerListHeader);

    // Scrollable peer list box
    auto* peerListBox = new brls::Box();
    peerListBox->setAxis(brls::Axis::COLUMN);
    peerListBox->setHeight(180.0f);
    peerListBox->setBackgroundColor(nvgRGBA(24, 26, 32, 200));
    peerListBox->setCornerRadius(8.0f);
    peerListBox->setPadding(8.0f);

    tsnx_peer_info peers[32];
    int peerCount = 0;
    if (!hash.empty()) {
        peerCount = tsnx_engine_get_peers(nullptr, hash.c_str(), peers, 32);
    }

    if (peerCount > 0) {
        for (int i = 0; i < peerCount; i++) {
            const auto& p = peers[i];
            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setHeight(26.0f);
            row->setMarginBottom(4.0f);

            char ipBuf[64];
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&p.ip);
            std::snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], p.port);

            auto* ipLabel = new brls::Label();
            ipLabel->setText(ipBuf);
            ipLabel->setFontSize(13.0f);
            ipLabel->setTextColor(nvgRGB(220, 220, 220));

            std::string statusStr;
            NVGcolor statusCol = nvgRGB(180, 180, 180);
            if (p.connecting) {
                statusStr = "Connecting...";
                statusCol = nvgRGB(255, 193, 7);
            } else if (p.claim_piece >= 0) {
                char speedBuf[32];
                std::snprintf(speedBuf, sizeof(speedBuf), "%.1f KB/s", p.rate_bps / 1024.0);
                statusStr = "Piece #" + std::to_string(p.claim_piece) + " (" + speedBuf + ")";
                statusCol = nvgRGB(76, 175, 80);
            } else if (p.choked) {
                statusStr = "Choked";
                statusCol = nvgRGB(244, 67, 54);
            } else {
                statusStr = "Idle (Unchoked)";
                statusCol = nvgRGB(33, 150, 243);
            }

            if (p.rtt_ms >= 0) {
                statusStr += " · " + std::to_string(p.rtt_ms) + "ms";
            }

            auto* statusLabel = new brls::Label();
            statusLabel->setText(statusStr);
            statusLabel->setFontSize(13.0f);
            statusLabel->setTextColor(statusCol);

            row->addView(ipLabel);
            row->addView(statusLabel);
            peerListBox->addView(row);
        }
    } else {
        auto* emptyLabel = new brls::Label();
        emptyLabel->setText("app/downloads/peers_no_active"_i18n);
        emptyLabel->setFontSize(14.0f);
        emptyLabel->setTextColor(nvgRGB(140, 140, 140));
        emptyLabel->setMarginTop(16.0f);
        peerListBox->addView(emptyLabel);
    }
    content->addView(peerListBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);
    dialog->addButton("common/ok"_i18n, []() {});
    dialog->open();
}

// DATASOURCE IMPLEMENTATION
int DownloadsView::DownloadsDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
    return ui::DownloadManager::instance().getImpl().queue().size();
}

void DownloadsView::updateCell(DownloadCell* cell, const download::DownloadItem& item) {
    cell->title->setText(cleanTitle(item.title));
    
    std::string coverUrl = findCoverForDownload(item);
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
    // Reset actions on cell to avoid accumulating stale actions across progress ticks
    cell->clearRegisteredActions();

    if (item.state == download::DownloadState::Completed || 
        item.state == download::DownloadState::Cancelled || 
        item.state == download::DownloadState::Failed) {
        
        cell->registerAction("app/downloads/action_delete"_i18n, brls::ControllerButton::BUTTON_Y, [topic_id = item.topic_id](brls::View* view) {
            ui::DownloadManager::instance().deleteDownload(topic_id);
            return true;
        });

        if (item.state == download::DownloadState::Completed && isFileDownloadItem(item)) {
            cell->registerAction("app/downloads/action_open_folder"_i18n, brls::ControllerButton::BUTTON_A, [item](brls::View* view) {
                openDownloadsFolderForItem(item);
                return true;
            }, false, false, brls::SOUND_CLICK);
        } else if (item.state == download::DownloadState::Failed || item.state == download::DownloadState::Cancelled) {
            cell->registerAction("app/downloads/action_retry"_i18n, brls::ControllerButton::BUTTON_A, [topic_id = item.topic_id](brls::View* view) {
                ui::DownloadManager::instance().retryDownload(topic_id);
                return true;
            }, false, false, brls::SOUND_CLICK);
        }
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

    // Swarm inspector action for all items
    cell->registerAction("app/downloads/action_peers"_i18n, brls::ControllerButton::BUTTON_LB, [item](brls::View* view) {
        showPeerInspector(item);
        return true;
    }, false, false, brls::SOUND_CLICK);

    // If this cell is currently focused, trigger hint refresh so bottom hints update immediately
    if (brls::Application::getCurrentFocus() == cell) {
        brls::Application::getGlobalHintsUpdateEvent()->fire();
    }
}


brls::RecyclerCell* DownloadsView::DownloadsDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    DownloadCell* cell = dynamic_cast<DownloadCell*>(recycler->dequeueReusableCell("Download"));
    if (!cell) return nullptr;

    int row = index.row;
    
    if (cell->imageToken) *(cell->imageToken) = false;
    cell->imageToken = std::make_shared<bool>(true);

    std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
    const auto& queue = ui::DownloadManager::instance().getImpl().queue();
    if (static_cast<size_t>(row) < queue.size()) {
        parent_->updateCell(cell, queue[row]);
    }

    cell->getFocusEvent()->subscribe([this, row](bool focused) {
        if (focused) {
            parent_->focusedRow_ = row;
        }
    });

    return cell;
}

void DownloadsView::DownloadsDataSource::didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
    const auto& queue = ui::DownloadManager::instance().getImpl().queue();
    if (static_cast<size_t>(index.row) < queue.size()) {
        const auto& item = queue[index.row];
        if (item.state == download::DownloadState::Completed && isFileDownloadItem(item)) {
            openDownloadsFolderForItem(item);
        } else if (item.state == download::DownloadState::Failed || item.state == download::DownloadState::Cancelled) {
            ui::DownloadManager::instance().retryDownload(item.topic_id);
        }
    }
}

} // namespace ui
