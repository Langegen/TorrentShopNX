#include "DownloadsView.hpp"
#include "DownloadUiManager.hpp"
#include "FileManagerView.hpp"
#include "ThemeManager.hpp"
#include "../config/config.h"
#include "../utils/switch_utils.h"
#include "../utils/screen_sleep_manager.h"
#include "../utils/app_paths.h"
#include "../utils/file_ops.h"
#include <engine/engine.h>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include "../utils/string_utils.h"

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

std::string getDownloadFileName(const download::DownloadItem& item) {
    std::string name = item.forced_stream_name;
    if (name.empty()) name = item.preload_stream_name;
    if (name.empty()) name = item.stream_name;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    return name;
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
}

void DownloadsView::onContentAvailable() {

    recycler->registerCell("Download", []() { return DownloadCell::create(); });
    recycler->setDataSource(new DownloadsDataSource(this));

    // Register hidden button action to toggle screen backlight (shown in top-right header instead of footer)
    this->registerAction("", brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        toggleBacklight();
        return true;
    }, true /* hidden from footer */);

    this->registerAction("", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
        const auto& queue = ui::DownloadManager::instance().getImpl().queue();
        if (focusedRow_ >= 0 && static_cast<size_t>(focusedRow_) < groups_.size()) {
            const auto& g = groups_[focusedRow_];
            size_t idx = (g.active_item_idx >= 0) ? (size_t)g.active_item_idx : (!g.item_indices.empty() ? g.item_indices.front() : 0);
            if (idx < queue.size()) {
                showPeerInspector(queue[idx]);
            }
        }
        return true;
    }, true /* hidden from footer */);

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_MINUS, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        toggleBacklight();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_I, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
        const auto& queue = ui::DownloadManager::instance().getImpl().queue();
        if (focusedRow_ >= 0 && static_cast<size_t>(focusedRow_) < groups_.size()) {
            const auto& g = groups_[focusedRow_];
            size_t idx = (g.active_item_idx >= 0) ? (size_t)g.active_item_idx : (!g.item_indices.empty() ? g.item_indices.front() : 0);
            if (idx < queue.size()) {
                showPeerInspector(queue[idx]);
            }
        }
        return true;
    });

    registerProgressCallback();

    // Force an immediate refresh so that cells show the current download state
    // right away after the auto-redirect from FileSelectView (without waiting for
    // the next progress-thread tick which can be up to 1 second away).
    ui::DownloadManager::instance().triggerCallback();
}

void DownloadsView::registerProgressCallback() {
    auto alive = alive_;
    ui::DownloadManager::instance().setProgressCallback([this, alive]() {
        if (!alive->load()) return;
        brls::sync([this, alive]() {
            if (!alive->load()) return;
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
            const auto& queue = ui::DownloadManager::instance().getImpl().queue();
            
            buildGroups(queue);

            if (groups_.empty()) {
                emptyLabel->setVisibility(brls::Visibility::VISIBLE);
                recycler->setVisibility(brls::Visibility::GONE);
            } else {
                emptyLabel->setVisibility(brls::Visibility::GONE);
                recycler->setVisibility(brls::Visibility::VISIBLE);
            }

            size_t currentRows = groups_.size();
            std::vector<std::string> currentOrder;
            currentOrder.reserve(groups_.size());
            for (const auto& g : groups_) currentOrder.push_back(g.group_id);

            bool orderChanged = (currentOrder != lastOrder_);

            // Check if recycler actually has populated cells
            bool hasCells = false;
            brls::Box* contentBox = nullptr;
            for (auto* child : recycler->getChildren()) {
                brls::Box* box = dynamic_cast<brls::Box*>(child);
                if (box) {
                    contentBox = box;
                    hasCells = !box->getChildren().empty();
                    break;
                }
            }

            if (currentRows != lastRows_ || orderChanged || (!hasCells && !groups_.empty())) {
                lastRows_ = currentRows;
                lastOrder_ = std::move(currentOrder);
                recycler->reloadData();
                if (!groups_.empty()) {
                    int validRow = std::clamp(focusedRow_, 0, static_cast<int>(groups_.size()) - 1);
                    recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                    recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                    brls::Application::giveFocus(recycler);
                }
                return;
            }

            // Update visible cells in-place to avoid focus jumping
            if (contentBox) {
                for (auto* cellView : contentBox->getChildren()) {
                    DownloadCell* cell = dynamic_cast<DownloadCell*>(cellView);
                    if (cell) {
                        int row = cell->getIndexPath().row;
                        if (row >= 0 && static_cast<size_t>(row) < groups_.size()) {
                            updateCell(cell, groups_[row], queue, row);
                        }
                    }
                }
            }

            // If focus was lost and queue is not empty, restore focus
            if (!groups_.empty() && brls::Application::getCurrentFocus() == nullptr) {
                int validRow = std::clamp(focusedRow_, 0, static_cast<int>(groups_.size()) - 1);
                recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                brls::Application::giveFocus(recycler);
            }
        });
    });
}

DownloadsView::~DownloadsView() {
    alive_->store(false);
    // Unregister callback on destruction to avoid crashes
    ui::DownloadManager::instance().setProgressCallback(nullptr);
}

void DownloadsView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    util::ScreenSleepManager::instance().resetActivity();
    registerProgressCallback();
    std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
    const auto& queue = ui::DownloadManager::instance().getImpl().queue();
    buildGroups(queue);

    if (groups_.empty()) {
        emptyLabel->setVisibility(brls::Visibility::VISIBLE);
        recycler->setVisibility(brls::Visibility::GONE);
    } else {
        emptyLabel->setVisibility(brls::Visibility::GONE);
        recycler->setVisibility(brls::Visibility::VISIBLE);
    }

    lastRows_ = groups_.size();
    lastOrder_.clear();
    for (const auto& g : groups_) lastOrder_.push_back(g.group_id);
    recycler->reloadData();

    if (!groups_.empty()) {
        int validRow = std::clamp(focusedRow_, 0, static_cast<int>(groups_.size()) - 1);
        recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
        recycler->selectRowAt(brls::IndexPath(0, validRow), false);
        brls::Application::giveFocus(recycler);
    }
}

void DownloadsView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    ui::DownloadManager::instance().setProgressCallback(nullptr);
}

void DownloadsView::toggleBacklight() {
    util::ScreenSleepManager::instance().toggleScreen();
}



static std::string formatKbps(float speed_kbps, bool is_download) {
    std::string prefix = is_download ? "↓ " : "";
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

static std::string formatFileCount(int n) {
    int n10 = n % 10;
    int n100 = n % 100;
    if (n100 >= 11 && n100 <= 19) return std::to_string(n) + " файлов";
    if (n10 == 1) return std::to_string(n) + " файл";
    if (n10 >= 2 && n10 <= 4) return std::to_string(n) + " файла";
    return std::to_string(n) + " файлов";
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

static std::string formatElapsedSeconds(int64_t secs) {
    if (secs <= 0) return "--:--";
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

static int64_t getItemElapsedSeconds(const download::DownloadItem& itm) {
    if (itm.state == download::DownloadState::Completed ||
        itm.state == download::DownloadState::Failed ||
        itm.state == download::DownloadState::Cancelled) {
        if (itm.elapsed_seconds > 0) return itm.elapsed_seconds;
        if (itm.finish_time.time_since_epoch().count() > 0 && itm.start_time.time_since_epoch().count() > 0) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(itm.finish_time - itm.start_time).count();
            return (sec > 0) ? sec : 1;
        }
        return 0;
    }
    if (itm.state == download::DownloadState::Paused) {
        return itm.elapsed_seconds;
    }
    if (itm.state == download::DownloadState::Downloading ||
        itm.state == download::DownloadState::StreamPreparing ||
        itm.state == download::DownloadState::StreamInstalling ||
        itm.state == download::DownloadState::Installing) {
        if (itm.start_time.time_since_epoch().count() > 0) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - itm.start_time).count();
            return itm.elapsed_seconds + ((sec > 0) ? sec : 0);
        }
        return itm.elapsed_seconds;
    }
    return 0;
}


static std::string formatProgressBytes(unsigned long long written, unsigned long long total) {
    if (total == 0) {
        if (written == 0) return "0 B";
        return formatBytes(written);
    }
    return formatBytes(written) + " / " + formatBytes(total);
}

static void showPeerInspector(const download::DownloadItem& item) {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    NVGcolor primaryText = ThemeManager::instance().getTextPrimaryColor();
    NVGcolor secondaryText = ThemeManager::instance().getTextSecondaryColor();

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(680.0f);
    content->setPadding(20.0f);

    // Header: Title
    auto* headerTitle = new brls::Label();
    headerTitle->setText("app/downloads/peers_title"_i18n);
    headerTitle->setFontSize(22.0f);
    headerTitle->setTextColor(primaryText);
    headerTitle->setMarginBottom(4.0f);
    content->addView(headerTitle);

    auto* subTitle = new brls::Label();
    subTitle->setText(cleanTitle(item.title));
    subTitle->setFontSize(14.0f);
    subTitle->setTextColor(secondaryText);
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
    statsBox->setBackgroundColor(isLight ? nvgRGBA(242, 245, 248, 255) : nvgRGBA(36, 39, 46, 180));
    statsBox->setCornerRadius(8.0f);
    statsBox->setPadding(12.0f);
    statsBox->setMarginBottom(16.0f);
    if (isLight) {
        statsBox->setBorderThickness(1.0f);
        statsBox->setBorderColor(nvgRGBA(215, 222, 232, 255));
    }

    auto addStatRow = [statsBox, primaryText, secondaryText](const std::string& label, const std::string& val) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setMarginBottom(4.0f);

        auto* l = new brls::Label();
        l->setText(label);
        l->setFontSize(14.0f);
        l->setTextColor(secondaryText);

        auto* v = new brls::Label();
        v->setText(val);
        v->setFontSize(14.0f);
        v->setTextColor(primaryText);

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
    peerListHeader->setTextColor(primaryText);
    peerListHeader->setMarginBottom(8.0f);
    content->addView(peerListHeader);

    // Scrollable peer list box
    auto* peerListBox = new brls::Box();
    peerListBox->setAxis(brls::Axis::COLUMN);
    peerListBox->setHeight(180.0f);
    peerListBox->setBackgroundColor(isLight ? nvgRGBA(242, 245, 248, 255) : nvgRGBA(24, 26, 32, 200));
    peerListBox->setCornerRadius(8.0f);
    peerListBox->setPadding(8.0f);
    if (isLight) {
        peerListBox->setBorderThickness(1.0f);
        peerListBox->setBorderColor(nvgRGBA(215, 222, 232, 255));
    }

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
            ipLabel->setTextColor(primaryText);

            std::string statusStr;
            NVGcolor statusCol = secondaryText;
            if (p.connecting) {
                statusStr = "Connecting...";
                statusCol = isLight ? nvgRGB(180, 110, 0) : nvgRGB(255, 193, 7);
            } else if (p.claim_piece >= 0) {
                char speedBuf[32];
                std::snprintf(speedBuf, sizeof(speedBuf), "%.1f KB/s", p.rate_bps / 1024.0);
                statusStr = "Piece #" + std::to_string(p.claim_piece) + " (" + speedBuf + ")";
                statusCol = isLight ? nvgRGB(34, 139, 34) : nvgRGB(76, 175, 80);
            } else if (p.choked) {
                statusStr = "Choked";
                statusCol = isLight ? nvgRGB(210, 40, 40) : nvgRGB(244, 67, 54);
            } else {
                statusStr = "Idle (Unchoked)";
                statusCol = isLight ? nvgRGB(20, 100, 200) : nvgRGB(33, 150, 243);
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
        emptyLabel->setTextColor(secondaryText);
        emptyLabel->setMarginTop(16.0f);
        peerListBox->addView(emptyLabel);
    }
    content->addView(peerListBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);
    dialog->addButton("app/common/ok"_i18n, []() {});

    auto* applet = dialog->getAppletFrame();
    if (applet) {
        applet->setWidth(720.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    dialog->open();
}

// THEME & ADAPTIVE STYLING HELPERS
static NVGcolor getDownloadStateColor(download::DownloadState state, bool isLight) {
    switch (state) {
        case download::DownloadState::Completed:
            return isLight ? nvgRGB(56, 142, 60) : nvgRGB(139, 195, 74);
        case download::DownloadState::StreamInstalling:
        case download::DownloadState::Installing:
            return isLight ? nvgRGB(25, 118, 210) : nvgRGB(33, 150, 243);
        case download::DownloadState::Downloading:
        case download::DownloadState::StreamPreparing:
            return isLight ? nvgRGB(46, 125, 50) : nvgRGB(76, 175, 80);
        case download::DownloadState::Queued:
            return isLight ? nvgRGB(190, 115, 0) : nvgRGB(255, 193, 7);
        case download::DownloadState::Paused:
            return isLight ? nvgRGB(216, 100, 0) : nvgRGB(255, 152, 0);
        case download::DownloadState::Failed:
        case download::DownloadState::Cancelled:
            return isLight ? nvgRGB(211, 47, 47) : nvgRGB(244, 67, 54);
        default:
            return isLight ? nvgRGB(100, 112, 128) : nvgRGB(180, 190, 200);
    }
}

static std::string getStatusString(download::DownloadState state) {
    switch (state) {
        case download::DownloadState::Queued:
            return "app/downloads/state_queued"_i18n;
        case download::DownloadState::Downloading:
            return "app/downloads/state_downloading"_i18n;
        case download::DownloadState::StreamPreparing:
            return "app/downloads/state_preparing"_i18n;
        case download::DownloadState::StreamInstalling:
        case download::DownloadState::Installing:
            return "app/downloads/state_installing"_i18n;
        case download::DownloadState::Completed:
            return "app/downloads/state_completed"_i18n;
        case download::DownloadState::Cancelled:
            return "app/downloads/state_cancelled"_i18n;
        case download::DownloadState::Failed:
            return "app/downloads/state_failed"_i18n;
        case download::DownloadState::Paused:
            return "app/downloads/state_paused"_i18n;
        default:
            return "app/downloads/state_waiting"_i18n;
    }
}

static int getGroupSortPriority(download::DownloadState state) {
    switch (state) {
        case download::DownloadState::Downloading:
        case download::DownloadState::StreamPreparing:
        case download::DownloadState::StreamInstalling:
        case download::DownloadState::Installing:
            return 0; // 1. Активные раздачи
        case download::DownloadState::Paused:
            return 1; // 2. На паузе
        case download::DownloadState::Queued:
            return 2; // 3. В очереди
        case download::DownloadState::Completed:
            return 3; // 4. Завершенные
        case download::DownloadState::Cancelled:
        case download::DownloadState::Failed:
        default:
            return 4; // 5. Отмененные / Ошибки
    }
}

struct ComponentBadgeStyle {
    std::string text;
    NVGcolor bg;
    NVGcolor textCol;
};

static ComponentBadgeStyle getComponentBadgeStyle(int component_type, bool isLight) {
    switch (component_type) {
        case 0: // Base Game
            return {
                "MAIN",
                isLight ? nvgRGBA(46, 125, 50, 40) : nvgRGBA(76, 175, 80, 50),
                isLight ? nvgRGB(46, 125, 50) : nvgRGB(129, 199, 132)
            };
        case 1: // Update
            return {
                "UPD",
                isLight ? nvgRGBA(25, 118, 210, 40) : nvgRGBA(33, 150, 243, 50),
                isLight ? nvgRGB(25, 118, 210) : nvgRGB(100, 181, 246)
            };
        case 2: // DLC
            return {
                "DLC",
                isLight ? nvgRGBA(142, 36, 170, 40) : nvgRGBA(186, 104, 200, 50),
                isLight ? nvgRGB(142, 36, 170) : nvgRGB(225, 190, 231)
            };
        case 3: // Mod / Translation
        case 4: // File / Port / Other
        default:
            return {
                "OTH",
                isLight ? nvgRGBA(216, 67, 21, 40) : nvgRGBA(255, 112, 67, 50),
                isLight ? nvgRGB(191, 54, 12) : nvgRGB(255, 171, 145)
            };
    }
}

void DownloadsView::buildGroups(const std::vector<download::DownloadItem>& queue) {
    std::map<std::string, std::pair<bool, bool>> state_map;
    for (const auto& g : groups_) {
        state_map[g.group_id] = { g.is_expanded, g.has_manual_toggle };
    }

    groups_.clear();
    std::map<std::string, size_t> group_index_map;

    for (size_t i = 0; i < queue.size(); ++i) {
        const auto& item = queue[i];
        std::string gid = item.group_id;
        if (gid.empty()) {
            gid = item.topic_id;
            size_t under = gid.rfind('_');
            if (under != std::string::npos && under > 0) {
                gid = gid.substr(0, under);
            }
        }

        auto it = group_index_map.find(gid);
        if (it == group_index_map.end()) {
            DownloadGroup g;
            g.group_id = gid;
            g.title = !item.group_title.empty() ? item.group_title : cleanTitle(item.title);
            if (!g.title.empty() && g.title.back() == '(') {
                g.title.pop_back();
                while (!g.title.empty() && std::isspace(static_cast<unsigned char>(g.title.back()))) {
                    g.title.pop_back();
                }
            }
            g.cover_url = item.cover_url;
            g.item_indices.push_back(i);

            auto st_it = state_map.find(gid);
            if (st_it != state_map.end()) {
                g.is_expanded = st_it->second.first;
                g.has_manual_toggle = st_it->second.second;
            }

            group_index_map[gid] = groups_.size();
            groups_.push_back(std::move(g));
        } else {
            groups_[it->second].item_indices.push_back(i);
        }
    }

    // Aggregate metrics
    for (auto& g : groups_) {
        g.dl_written = 0;
        g.dl_total = 0;
        g.inst_written = 0;
        g.inst_total = 0;
        g.dl_speed_kbps = 0.0f;
        g.inst_speed_kbps = 0.0f;
        g.seeds = 0;
        g.peers = 0;
        g.known_peers = 0;
        g.dht = 0;
        g.completed_count = 0;
        g.total_count = static_cast<int>(g.item_indices.size());
        g.failed_item_idx = -1;
        g.active_item_idx = -1;
        g.failed_name.clear();
        g.failed_error.clear();
        g.start_time = std::chrono::steady_clock::time_point{};
        g.finish_time = std::chrono::steady_clock::time_point{};
        g.elapsed_seconds = 0;
        int64_t group_total_elapsed = 0;

        bool has_active = false;
        bool has_paused = false;
        bool has_failed = false;

        for (size_t qidx : g.item_indices) {
            const auto& itm = queue[qidx];
            if (g.cover_url.empty() && !itm.cover_url.empty()) {
                g.cover_url = itm.cover_url;
            }
            if (itm.start_time.time_since_epoch().count() > 0) {
                if (g.start_time.time_since_epoch().count() == 0 || itm.start_time < g.start_time) {
                    g.start_time = itm.start_time;
                }
            }
            if (itm.finish_time.time_since_epoch().count() > 0) {
                if (g.finish_time.time_since_epoch().count() == 0 || itm.finish_time > g.finish_time) {
                    g.finish_time = itm.finish_time;
                }
            }
            group_total_elapsed += getItemElapsedSeconds(itm);

            if (itm.is_port_auto_install) {
                g.has_port_install = true;
            }

            // Download bytes
            unsigned long long item_dl_w = 0;
            unsigned long long item_dl_t = 0;
            if (itm.hybrid_installer) {
                item_dl_w = itm.hybrid_installer->bytesDownloaded();
                item_dl_t = itm.hybrid_installer->downloadTotalBytes();
                if (item_dl_t == 0) item_dl_t = itm.download_total_bytes;
            } else if (itm.is_port_auto_install && itm.port_install_state) {
                item_dl_w = itm.port_install_state->dl_written.load();
                item_dl_t = itm.port_install_state->dl_total.load();
                if (item_dl_t == 0) item_dl_t = itm.download_total_bytes;
                if (item_dl_w == 0 && itm.state == download::DownloadState::Completed) {
                    item_dl_w = item_dl_t;
                }
            } else if (itm.file_dl_state) {
                item_dl_w = itm.file_dl_state->written.load();
                item_dl_t = itm.file_dl_state->total.load();
                if (item_dl_t == 0) item_dl_t = itm.file_dl_state->size;
                if (item_dl_t == 0) item_dl_t = itm.download_total_bytes;
            } else {
                item_dl_t = itm.download_total_bytes > 0 ? itm.download_total_bytes : itm.install_total;
                if (itm.state == download::DownloadState::Completed) {
                    item_dl_w = item_dl_t;
                } else if (itm.bytes_downloaded > 0) {
                    item_dl_w = itm.bytes_downloaded;
                } else {
                    float norm_prog = std::clamp(itm.progress, 0.0f, 1.0f);
                    item_dl_w = (item_dl_t > 0) ? static_cast<unsigned long long>(static_cast<double>(item_dl_t) * norm_prog) : 0;
                }
            }
            g.dl_written += item_dl_w;
            g.dl_total += item_dl_t;

            // Install bytes
            unsigned long long item_inst_w = 0;
            unsigned long long item_inst_t = 0;
            if (itm.hybrid_installer) {
                item_inst_w = itm.hybrid_installer->bytesInstalled();
                item_inst_t = itm.hybrid_installer->totalBytes();
            } else if (itm.is_port_auto_install && itm.port_install_state) {
                item_inst_w = itm.port_install_state->inst_written.load();
                item_inst_t = itm.port_install_state->inst_total.load();
            } else {
                item_inst_w = itm.install_written;
                item_inst_t = itm.install_total;
            }
            if (itm.state == download::DownloadState::Completed) {
                if (item_inst_t > 0) item_inst_w = item_inst_t;
            }
            g.inst_written += item_inst_w;
            g.inst_total += item_inst_t;

            if (itm.state == download::DownloadState::Completed) {
                g.completed_count++;
            } else if (itm.state == download::DownloadState::Failed || itm.state == download::DownloadState::Cancelled) {
                has_failed = true;
                if (g.failed_item_idx < 0) {
                    g.failed_item_idx = static_cast<int>(qidx);
                    g.failed_name = !itm.component_name.empty() ? itm.component_name : itm.title;
                    g.failed_error = itm.error_message;
                }
            } else if (itm.state == download::DownloadState::Downloading ||
                       itm.state == download::DownloadState::StreamPreparing ||
                       itm.state == download::DownloadState::StreamInstalling ||
                       itm.state == download::DownloadState::Installing) {
                has_active = true;
                g.active_item_idx = static_cast<int>(qidx);
                g.dl_speed_kbps = itm.download_speed_kbps;
                g.inst_speed_kbps = itm.install_speed_kbps;
                g.seeds = itm.seeds;
                g.peers = itm.peers;
                g.known_peers = itm.known_peers;
                g.dht = itm.dht;
            } else if (itm.state == download::DownloadState::Paused) {
                has_paused = true;
            }
        }

        if (has_active) {
            const auto& act = queue[g.active_item_idx];
            g.state = act.state;
            g.elapsed_seconds = group_total_elapsed;
        } else if (has_failed) {
            g.state = download::DownloadState::Failed;
            g.elapsed_seconds = group_total_elapsed;
        } else if (has_paused) {
            g.state = download::DownloadState::Paused;
            g.elapsed_seconds = group_total_elapsed;
        } else if (g.completed_count == g.total_count && g.total_count > 0) {
            g.state = download::DownloadState::Completed;
            if (g.finish_time.time_since_epoch().count() > 0 && g.start_time.time_since_epoch().count() > 0) {
                auto wallSec = std::chrono::duration_cast<std::chrono::seconds>(g.finish_time - g.start_time).count();
                g.elapsed_seconds = (wallSec > 0) ? wallSec : group_total_elapsed;
            } else {
                g.elapsed_seconds = group_total_elapsed;
            }
            if (g.elapsed_seconds <= 0) g.elapsed_seconds = 1;
        } else {
            g.state = download::DownloadState::Queued;
            g.elapsed_seconds = group_total_elapsed;
        }

        // Auto expansion logic:
        if (!g.has_manual_toggle) {
            if (has_active || has_failed) {
                g.is_expanded = true;
            } else if (g.state == download::DownloadState::Completed) {
                g.is_expanded = false;
            }
        }
    }

    // Sort groups by status priority:
    // 1. Active (Downloading/Installing) -> 2. Paused -> 3. Queued -> 4. Completed -> 5. Cancelled/Failed
    std::stable_sort(groups_.begin(), groups_.end(), [](const DownloadGroup& a, const DownloadGroup& b) {
        return getGroupSortPriority(a.state) < getGroupSortPriority(b.state);
    });
}

// DATASOURCE IMPLEMENTATION
int DownloadsView::DownloadsDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return parent_->groups_.size();
}

float DownloadsView::DownloadsDataSource::heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    int row = index.row;
    if (row < 0 || static_cast<size_t>(row) >= parent_->groups_.size()) return 138.0f;
    const auto& group = parent_->groups_[row];
    if (!group.is_expanded || group.total_count <= 1) return 138.0f;
    return 138.0f + 12.0f + static_cast<float>(group.total_count) * 50.0f;
}

void DownloadsView::updateCell(DownloadCell* cell, const DownloadGroup& group, const std::vector<download::DownloadItem>& queue, int row) {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    NVGcolor primaryText = ThemeManager::instance().getTextPrimaryColor();
    NVGcolor secondaryText = ThemeManager::instance().getTextSecondaryColor();

    cell->title->setText(group.title);

    // Count badge
    if (group.total_count > 1) {
        std::string badgeStr = brls::getStr("app/downloads/count_badge", std::to_string(group.completed_count), std::to_string(group.total_count));
        cell->countBadge->setText(badgeStr);
        cell->countBadge->setTextColor(secondaryText);
        cell->countBadge->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->countBadge->setVisibility(brls::Visibility::GONE);
    }

    // Cover art
    std::string coverUrl = group.cover_url;
    if (coverUrl.empty() && !group.item_indices.empty()) {
        coverUrl = findCoverForDownload(queue[group.item_indices.front()]);
    }
    if (!coverUrl.empty()) {
        if (cell->loadedCoverUrl != coverUrl) {
            cell->loadedCoverUrl = coverUrl;
            setImageFromHTTPS(cell->cover, coverUrl, cell->imageToken);
        }
    } else {
        if (cell->loadedCoverUrl != "__fallback__") {
            cell->loadedCoverUrl = "__fallback__";
            cell->cover->setImageFromFile("romfs:/img/borealis_96.png");
        }
    }

    // Overall Download Progress
    float dl_progress = (group.dl_total > 0) ? std::clamp(static_cast<float>(static_cast<double>(group.dl_written) / static_cast<double>(group.dl_total)), 0.0f, 1.0f) : 0.0f;
    cell->progressBar->setWidth(dl_progress * 275.0f);

    char progressBuf[32];
    std::snprintf(progressBuf, sizeof(progressBuf), "%d%%", static_cast<int>(dl_progress * 100.0f));
    std::string progressStr = progressBuf;

    if (group.state == download::DownloadState::Downloading || 
        group.state == download::DownloadState::StreamInstalling) {
        progressStr += "  (" + formatKbps(group.dl_speed_kbps, true) + ", " + formatProgressBytes(group.dl_written, group.dl_total) + ")";
    } else {
        progressStr += "  (" + formatProgressBytes(group.dl_written, group.dl_total) + ")";
    }
    cell->progressText->setText(progressStr);

    // Overall Install Progress
    const bool showsInstall = ((group.inst_total > 0 || group.has_port_install) &&
                               (group.state == download::DownloadState::StreamPreparing || 
                                group.state == download::DownloadState::StreamInstalling || 
                                group.state == download::DownloadState::Installing ||
                                group.state == download::DownloadState::Completed ||
                                (group.has_port_install && group.state != download::DownloadState::Queued)));
    
    if (showsInstall) {
        cell->installProgressRow->setVisibility(brls::Visibility::VISIBLE);
        float instProgress = (group.inst_total > 0) ? std::clamp(static_cast<float>(static_cast<double>(group.inst_written) / static_cast<double>(group.inst_total)), 0.0f, 1.0f) : 0.0f;
        cell->installBar->setWidth(instProgress * 275.0f);

        char instBuf[32];
        std::snprintf(instBuf, sizeof(instBuf), "%d%%", static_cast<int>(instProgress * 100.0f));
        std::string instStr = instBuf;

        if (group.state == download::DownloadState::StreamInstalling ||
            group.state == download::DownloadState::Installing ||
            (group.has_port_install && group.inst_written > 0 && group.state != download::DownloadState::Completed)) {
            if (group.inst_speed_kbps > 0.0f) {
                instStr += "  (" + formatKbps(group.inst_speed_kbps, false) + ", " + formatProgressBytes(group.inst_written, group.inst_total) + ")";
            } else {
                instStr += "  (" + formatProgressBytes(group.inst_written, group.inst_total) + ")";
            }
        } else {
            instStr += "  (" + formatProgressBytes(group.inst_written, group.inst_total) + ")";
        }
        cell->installText->setText(instStr);
    } else {
        cell->installProgressRow->setVisibility(brls::Visibility::GONE);
    }

    // Status description
    std::string statusStr = getStatusString(group.state);
    if (group.state == download::DownloadState::Failed && !group.failed_name.empty()) {
        statusStr = "Ошибка (" + group.failed_name + ")";
    }
    cell->statusText->setText(statusStr);
    cell->statusText->setTextColor(getDownloadStateColor(group.state, isLight));

    // Stats text (ETA, stage label, etc.)
    std::string stageMsg;
    if (group.active_item_idx >= 0 && static_cast<size_t>(group.active_item_idx) < queue.size()) {
        const auto& act = queue[group.active_item_idx];
        if (!act.install_stage_label.empty()) {
            stageMsg = act.install_stage_label;
        }
    }

    if (!stageMsg.empty() && group.state != download::DownloadState::Completed && group.state != download::DownloadState::Failed) {
        if (group.state == download::DownloadState::Downloading || 
            group.state == download::DownloadState::StreamInstalling) {
            unsigned long long remaining_dl = (group.dl_total >= group.dl_written) ? (group.dl_total - group.dl_written) : 0;
            std::string eta = formatEta(group.dl_speed_kbps, remaining_dl);
            if (!eta.empty()) {
                stageMsg += " · " + eta;
            }
        }
        cell->statsText->setText(stageMsg);
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else if (group.state == download::DownloadState::Downloading || 
        group.state == download::DownloadState::StreamInstalling) {
        unsigned long long remaining_dl = (group.dl_total >= group.dl_written) ? (group.dl_total - group.dl_written) : 0;
        cell->statsText->setText(formatEta(group.dl_speed_kbps, remaining_dl));
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else if (group.state == download::DownloadState::Completed) {
        cell->statsText->setText("app/downloads/installed"_i18n);
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else if (group.state == download::DownloadState::Failed) {
        cell->statsText->setText(group.failed_error.empty() ? "app/downloads/unknown_error"_i18n : group.failed_error);
        cell->statsText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->statsText->setVisibility(brls::Visibility::GONE);
    }

    // Elapsed timer
    const bool showsElapsed =
        group.state == download::DownloadState::Downloading ||
        group.state == download::DownloadState::StreamPreparing ||
        group.state == download::DownloadState::StreamInstalling ||
        group.state == download::DownloadState::Installing ||
        group.state == download::DownloadState::Completed ||
        (group.elapsed_seconds > 0);
    if (showsElapsed && group.elapsed_seconds > 0) {
        cell->elapsedText->setText(brls::getStr("app/downloads/elapsed", formatElapsedSeconds(group.elapsed_seconds)));
        cell->elapsedText->setVisibility(brls::Visibility::VISIBLE);
    } else if (showsElapsed && group.start_time.time_since_epoch().count() > 0) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - group.start_time).count();
        cell->elapsedText->setText(brls::getStr("app/downloads/elapsed", formatElapsedSeconds(secs)));
        cell->elapsedText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->elapsedText->setVisibility(brls::Visibility::GONE);
    }

    // Peers
    if (group.state == download::DownloadState::Downloading || 
        group.state == download::DownloadState::StreamInstalling ||
        group.state == download::DownloadState::StreamPreparing) {
        std::string peersStr;
        if (group.known_peers > group.peers) {
            peersStr = brls::getStr("app/downloads/peers_with_known", std::to_string(group.seeds), std::to_string(group.peers), std::to_string(group.known_peers), std::to_string(group.dht));
        } else {
            peersStr = brls::getStr("app/downloads/peers_standard", std::to_string(group.seeds), std::to_string(group.peers), std::to_string(group.dht));
        }
        cell->peersText->setText(peersStr);
        cell->peersText->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->peersText->setVisibility(brls::Visibility::GONE);
    }

    // Expand hint toggle label
    if (group.total_count > 1) {
        cell->expandHint->setText(group.is_expanded ? "▲ Свернуть" : ("▼ " + formatFileCount(group.total_count)));
        cell->expandHint->setTextColor(isLight ? nvgRGB(0, 140, 225) : nvgRGB(0, 200, 245));
        cell->expandHint->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->expandHint->setVisibility(brls::Visibility::GONE);
    }

    // SDK Mismatch warning badge
    if (cell->sdkWarningBox) {
        bool hasSdkMismatch = false;
        util::SdkVersion cSdk, gSdk;
        for (size_t qidx : group.item_indices) {
            if (qidx < queue.size() && queue[qidx].sdk_mismatch && queue[qidx].game_sdk.valid) {
                hasSdkMismatch = true;
                cSdk = queue[qidx].console_sdk;
                gSdk = queue[qidx].game_sdk;
                break;
            }
        }
        if (hasSdkMismatch) {
            cell->sdkWarningBox->setVisibility(brls::Visibility::VISIBLE);
            cell->sdkWarningBox->setBackgroundColor(isLight ? nvgRGBA(255, 235, 238, 240) : nvgRGBA(244, 67, 54, 40));
            cell->sdkWarningBox->setCornerRadius(6.0f);
            cell->sdkWarningBox->setPadding(4.0f, 8.0f, 4.0f, 8.0f);
            cell->sdkWarningBox->setBorderThickness(1.0f);
            cell->sdkWarningBox->setBorderColor(isLight ? nvgRGBA(239, 83, 80, 200) : nvgRGBA(255, 112, 67, 180));
            if (cell->sdkConsoleLabel) {
                cell->sdkConsoleLabel->setText("FW: " + (cSdk.valid ? cSdk.toString() : "?"));
                cell->sdkConsoleLabel->setTextColor(secondaryText);
            }
            if (cell->sdkGameLabel) {
                cell->sdkGameLabel->setText("\uE002 SDK: " + gSdk.toString());
                cell->sdkGameLabel->setTextColor(isLight ? nvgRGB(211, 47, 47) : nvgRGB(255, 112, 67));
            }
        } else {
            cell->sdkWarningBox->setVisibility(brls::Visibility::GONE);
        }
    }

    // Accordion Component List Box
    if (group.is_expanded && group.total_count > 1) {
        cell->componentsBox->setVisibility(brls::Visibility::VISIBLE);

        NVGcolor compBg = isLight ? nvgRGBA(242, 245, 248, 220) : nvgRGBA(32, 35, 42, 180);
        NVGcolor compBorder = isLight ? nvgRGBA(210, 218, 228, 200) : nvgRGBA(255, 255, 255, 25);

        // Rebuild rows if structure changed or count mismatch
        bool needsRebuild = (cell->componentsBox->getChildren().size() != group.item_indices.size());
        if (!needsRebuild) {
            for (auto* ch : cell->componentsBox->getChildren()) {
                auto* rb = dynamic_cast<brls::Box*>(ch);
                if (!rb || rb->getChildren().size() != 3) {
                    needsRebuild = true;
                    break;
                }
            }
        }

        if (needsRebuild) {
            cell->componentsBox->clearViews();
            for (size_t k = 0; k < group.item_indices.size(); ++k) {
                auto* rowBox = new brls::Box();
                rowBox->setAxis(brls::Axis::ROW);
                rowBox->setAlignItems(brls::AlignItems::CENTER);
                rowBox->setHeight(46.0f);
                rowBox->setCornerRadius(6.0f);
                rowBox->setPadding(6.0f, 10.0f, 6.0f, 10.0f);
                rowBox->setMarginBottom(4.0f);

                // 0. Left Info Box (fixed width 290px): [Badge] [Name] [SDK pill]
                auto* infoBox = new brls::Box();
                infoBox->setAxis(brls::Axis::ROW);
                infoBox->setAlignItems(brls::AlignItems::CENTER);
                infoBox->setWidth(290.0f);
                infoBox->setMarginRight(10.0f);

                // Badge Box
                auto* bBox = new brls::Box();
                bBox->setWidth(48.0f);
                bBox->setHeight(22.0f);
                bBox->setJustifyContent(brls::JustifyContent::CENTER);
                bBox->setAlignItems(brls::AlignItems::CENTER);
                bBox->setCornerRadius(4.0f);
                bBox->setMarginRight(8.0f);
                auto* bLabel = new brls::Label();
                bLabel->setFontSize(11.0f);
                bLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
                bLabel->setSingleLine(true);
                bBox->addView(bLabel);
                infoBox->addView(bBox);

                // Name Label
                auto* nameLabel = new brls::Label();
                nameLabel->setFontSize(13.0f);
                nameLabel->setSingleLine(true);
                nameLabel->setGrow(1.0f);
                nameLabel->setShrink(1.0f);
                nameLabel->setMarginRight(6.0f);
                infoBox->addView(nameLabel);

                // SDK Warning Pill
                auto* sdkPill = new brls::Box();
                sdkPill->setCornerRadius(4.0f);
                sdkPill->setPadding(2.0f, 5.0f, 2.0f, 5.0f);
                sdkPill->setVisibility(brls::Visibility::GONE);
                auto* sdkPillLabel = new brls::Label();
                sdkPillLabel->setFontSize(10.0f);
                sdkPillLabel->setSingleLine(true);
                sdkPill->addView(sdkPillLabel);
                infoBox->addView(sdkPill);

                rowBox->addView(infoBox);

                                // 1. Middle Progress Column (grow 1.0)
                auto* progCol = new brls::Box();
                progCol->setAxis(brls::Axis::COLUMN);
                progCol->setJustifyContent(brls::JustifyContent::CENTER);
                progCol->setGrow(1.0f);
                progCol->setMarginRight(12.0f);

                // Summary row (for queued / completed / idle states)
                auto* summaryRow = new brls::Box();
                summaryRow->setAxis(brls::Axis::ROW);
                summaryRow->setAlignItems(brls::AlignItems::CENTER);
                auto* summaryTxt = new brls::Label();
                summaryTxt->setFontSize(12.0f);
                summaryTxt->setSingleLine(true);
                summaryRow->addView(summaryTxt);
                progCol->addView(summaryRow);

                // Download mini-bar row
                auto* dlRow = new brls::Box();
                dlRow->setAxis(brls::Axis::ROW);
                dlRow->setAlignItems(brls::AlignItems::CENTER);
                dlRow->setMarginBottom(2.0f);
                auto* dlLbl = new brls::Label();
                dlLbl->setText("app/downloads/download_label"_i18n);
                dlLbl->setFontSize(10.0f);
                dlLbl->setWidth(70.0f);
                auto* dlBarBg = new brls::Box();
                dlBarBg->setWidth(110.0f);
                dlBarBg->setHeight(6.0f);
                dlBarBg->setCornerRadius(3.0f);
                dlBarBg->setBackgroundColor(nvgRGB(50, 50, 50));
                auto* dlBar = new brls::Box();
                dlBar->setWidth(0.0f);
                dlBar->setHeight(6.0f);
                dlBar->setCornerRadius(3.0f);
                dlBar->setBackgroundColor(nvgRGB(76, 175, 80));
                dlBarBg->addView(dlBar);
                auto* dlTxt = new brls::Label();
                dlTxt->setFontSize(10.0f);
                dlTxt->setMarginLeft(8.0f);
                dlTxt->setWidth(165.0f);
                dlTxt->setSingleLine(true);
                dlRow->addView(dlLbl);
                dlRow->addView(dlBarBg);
                dlRow->addView(dlTxt);
                progCol->addView(dlRow);

                // Install mini-bar row
                auto* instRow = new brls::Box();
                instRow->setAxis(brls::Axis::ROW);
                instRow->setAlignItems(brls::AlignItems::CENTER);
                auto* instLbl = new brls::Label();
                instLbl->setText("app/downloads/install_label"_i18n);
                instLbl->setFontSize(10.0f);
                instLbl->setWidth(70.0f);
                auto* instBarBg = new brls::Box();
                instBarBg->setWidth(110.0f);
                instBarBg->setHeight(6.0f);
                instBarBg->setCornerRadius(3.0f);
                instBarBg->setBackgroundColor(nvgRGB(50, 50, 50));
                auto* instBar = new brls::Box();
                instBar->setWidth(0.0f);
                instBar->setHeight(6.0f);
                instBar->setCornerRadius(3.0f);
                instBar->setBackgroundColor(isLight ? nvgRGB(25, 118, 210) : nvgRGB(33, 150, 243));
                instBarBg->addView(instBar);
                auto* instTxt = new brls::Label();
                instTxt->setFontSize(10.0f);
                instTxt->setMarginLeft(8.0f);
                instTxt->setWidth(165.0f);
                instTxt->setSingleLine(true);
                instRow->addView(instLbl);
                instRow->addView(instBarBg);
                instRow->addView(instTxt);
                progCol->addView(instRow);

                rowBox->addView(progCol);

                // 2. Right Status & Time Column (fixed 155px, vertical stack)
                auto* statusCol = new brls::Box();
                statusCol->setAxis(brls::Axis::COLUMN);
                statusCol->setAlignItems(brls::AlignItems::FLEX_END);
                statusCol->setJustifyContent(brls::JustifyContent::CENTER);
                statusCol->setWidth(155.0f);

                // Status Label
                auto* compStatusLabel = new brls::Label();
                compStatusLabel->setFontSize(12.0f);
                compStatusLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
                compStatusLabel->setSingleLine(true);
                statusCol->addView(compStatusLabel);

                // Elapsed Time Label
                auto* compTimeLabel = new brls::Label();
                compTimeLabel->setFontSize(10.0f);
                compTimeLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
                compTimeLabel->setSingleLine(true);
                compTimeLabel->setMarginTop(2.0f);
                statusCol->addView(compTimeLabel);

                rowBox->addView(statusCol);

                cell->componentsBox->addView(rowBox);
            }
        }

        // Populate component rows
        for (size_t i = 0; i < group.item_indices.size(); ++i) {
            size_t qidx = group.item_indices[i];
            if (qidx >= queue.size()) continue;
            const auto& itm = queue[qidx];
            auto* rowBox = dynamic_cast<brls::Box*>(cell->componentsBox->getChildren()[i]);
            if (!rowBox || rowBox->getChildren().size() < 3) continue;

            auto* infoBox = dynamic_cast<brls::Box*>(rowBox->getChildren()[0]);
            auto* progCol = dynamic_cast<brls::Box*>(rowBox->getChildren()[1]);
            auto* statusCol = dynamic_cast<brls::Box*>(rowBox->getChildren()[2]);
            if (!infoBox || !progCol || !statusCol) continue;

            bool isFailed = (itm.state == download::DownloadState::Failed || itm.state == download::DownloadState::Cancelled);

            if (isFailed) {
                rowBox->setBackgroundColor(isLight ? nvgRGBA(255, 235, 238, 220) : nvgRGBA(244, 67, 54, 35));
                rowBox->setBorderThickness(1.0f);
                rowBox->setBorderColor(isLight ? nvgRGB(211, 47, 47) : nvgRGB(244, 67, 54));
            } else {
                rowBox->setBackgroundColor(compBg);
                rowBox->setBorderThickness(1.0f);
                rowBox->setBorderColor(compBorder);
            }

            // 0. Info Box: Badge
            auto* bBox = dynamic_cast<brls::Box*>(infoBox->getChildren()[0]);
            auto* bLabel = bBox ? dynamic_cast<brls::Label*>(bBox->getChildren()[0]) : nullptr;
            auto bStyle = getComponentBadgeStyle(itm.component_type, isLight);
            if (bBox && bLabel) {
                bBox->setBackgroundColor(bStyle.bg);
                std::string badgeText = itm.component_label;
                if (badgeText.empty() || badgeText == "Игра" || badgeText == "Базовая игра") badgeText = "MAIN";
                else if (badgeText == "Обновление" || badgeText == "Патч") badgeText = "UPD";
                else if (badgeText == "DLC" || badgeText == "Дополнение") badgeText = "DLC";
                else if (badgeText == "Русификатор" || badgeText == "Порт" || badgeText == "Файл" || badgeText == "Бандл") badgeText = "OTH";
                else if (badgeText != "MAIN" && badgeText != "UPD" && badgeText != "DLC" && badgeText != "OTH") {
                    badgeText = bStyle.text;
                }
                bLabel->setText(badgeText);
                bLabel->setTextColor(bStyle.textCol);
            }

            // 0. Info Box: Name
            auto* nameLabel = dynamic_cast<brls::Label*>(infoBox->getChildren()[1]);
            if (nameLabel) {
                std::string cname = itm.component_name;
                if (cname.empty() || cname == "Патч") {
                    std::string fname = getDownloadFileName(itm);
                    auto comp = util::classifyDownloadComponent(fname, itm.title);
                    if (!comp.cleanName.empty()) {
                        cname = comp.cleanName;
                    } else if (cname.empty()) {
                        cname = fname;
                    }
                }
                nameLabel->setText(cname);
                nameLabel->setTextColor(primaryText);
            }

            // 0. Info Box: SDK Warning Pill
            auto* sdkPill = dynamic_cast<brls::Box*>(infoBox->getChildren()[2]);
            if (sdkPill) {
                if (itm.sdk_mismatch && itm.game_sdk.valid) {
                    sdkPill->setVisibility(brls::Visibility::VISIBLE);
                    sdkPill->setBackgroundColor(isLight ? nvgRGBA(255, 235, 238, 240) : nvgRGBA(244, 67, 54, 40));
                    sdkPill->setBorderThickness(1.0f);
                    sdkPill->setBorderColor(isLight ? nvgRGBA(239, 83, 80, 200) : nvgRGBA(255, 112, 67, 180));
                    auto* sdkPillLabel = dynamic_cast<brls::Label*>(sdkPill->getChildren()[0]);
                    if (sdkPillLabel) {
                        sdkPillLabel->setText("SDK " + itm.game_sdk.toString());
                        sdkPillLabel->setTextColor(isLight ? nvgRGB(211, 47, 47) : nvgRGB(255, 112, 67));
                    }
                } else {
                    sdkPill->setVisibility(brls::Visibility::GONE);
                }
            }

            // Calculate component sizes
            unsigned long long itm_dl_w = 0;
            unsigned long long itm_dl_t = 0;
            if (itm.hybrid_installer) {
                itm_dl_w = itm.hybrid_installer->bytesDownloaded();
                itm_dl_t = itm.hybrid_installer->downloadTotalBytes();
                if (itm_dl_t == 0) itm_dl_t = itm.download_total_bytes;
            } else if (itm.is_port_auto_install && itm.port_install_state) {
                itm_dl_w = itm.port_install_state->dl_written.load();
                itm_dl_t = itm.port_install_state->dl_total.load();
                if (itm_dl_t == 0) itm_dl_t = itm.download_total_bytes;
                if (itm_dl_w == 0 && itm.state == download::DownloadState::Completed) {
                    itm_dl_w = itm_dl_t;
                }
            } else if (itm.file_dl_state) {
                itm_dl_w = itm.file_dl_state->written.load();
                itm_dl_t = itm.file_dl_state->total.load();
                if (itm_dl_t == 0) itm_dl_t = itm.file_dl_state->size;
                if (itm_dl_t == 0) itm_dl_t = itm.download_total_bytes;
            } else {
                itm_dl_t = itm.download_total_bytes > 0 ? itm.download_total_bytes : itm.install_total;
                if (itm.state == download::DownloadState::Completed) {
                    itm_dl_w = itm_dl_t;
                } else if (itm.bytes_downloaded > 0) {
                    itm_dl_w = itm.bytes_downloaded;
                } else {
                    float norm_prog = std::clamp(itm.progress, 0.0f, 1.0f);
                    itm_dl_w = (itm_dl_t > 0) ? static_cast<unsigned long long>(static_cast<double>(itm_dl_t) * norm_prog) : 0;
                }
            }

            unsigned long long itm_inst_w = 0;
            unsigned long long itm_inst_t = 0;
            if (itm.hybrid_installer) {
                itm_inst_w = itm.hybrid_installer->bytesInstalled();
                itm_inst_t = itm.hybrid_installer->totalBytes();
            } else if (itm.is_port_auto_install && itm.port_install_state) {
                itm_inst_w = itm.port_install_state->inst_written.load();
                itm_inst_t = itm.port_install_state->inst_total.load();
            } else {
                itm_inst_w = itm.install_written;
                itm_inst_t = itm.install_total;
            }
            if (itm.state == download::DownloadState::Completed && itm_inst_t > 0) {
                itm_inst_w = itm_inst_t;
            }

            // 1. Progress Col
            bool isActive = (itm.state == download::DownloadState::Downloading ||
                             itm.state == download::DownloadState::StreamPreparing ||
                             itm.state == download::DownloadState::StreamInstalling ||
                             itm.state == download::DownloadState::Installing);

            auto* summaryRow = dynamic_cast<brls::Box*>(progCol->getChildren()[0]);
            auto* summaryTxt = summaryRow ? dynamic_cast<brls::Label*>(summaryRow->getChildren()[0]) : nullptr;
            auto* dlRow = dynamic_cast<brls::Box*>(progCol->getChildren()[1]);
            auto* instRow = dynamic_cast<brls::Box*>(progCol->getChildren()[2]);

            if (!isActive) {
                if (summaryRow) summaryRow->setVisibility(brls::Visibility::VISIBLE);
                if (dlRow) dlRow->setVisibility(brls::Visibility::GONE);
                if (instRow) instRow->setVisibility(brls::Visibility::GONE);

                if (summaryTxt) {
                    if (itm.state == download::DownloadState::Completed) {
                        unsigned long long displaySize = (itm_inst_t > 0) ? itm_inst_t : itm_dl_t;
                        summaryTxt->setText(displaySize > 0 ? formatBytes(displaySize) : "");
                        summaryTxt->setTextColor(isLight ? nvgRGB(46, 125, 50) : nvgRGB(129, 199, 132));
                    } else if (itm.state == download::DownloadState::Queued) {
                        unsigned long long displaySize = (itm_dl_t > 0) ? itm_dl_t : itm_inst_t;
                        summaryTxt->setText("app/downloads/state_queued"_i18n + (displaySize > 0 ? (" · " + formatBytes(displaySize)) : ""));
                        summaryTxt->setTextColor(secondaryText);
                    } else if (itm.state == download::DownloadState::Paused) {
                        summaryTxt->setText("app/downloads/state_paused"_i18n + (itm_dl_t > 0 ? (" · " + formatProgressBytes(itm_dl_w, itm_dl_t)) : ""));
                        summaryTxt->setTextColor(secondaryText);
                    } else if (isFailed) {
                        summaryTxt->setText(itm.error_message.empty() ? "app/downloads/state_failed"_i18n : itm.error_message);
                        summaryTxt->setTextColor(isLight ? nvgRGB(211, 47, 47) : nvgRGB(244, 67, 54));
                    } else {
                        summaryTxt->setText(getStatusString(itm.state));
                        summaryTxt->setTextColor(secondaryText);
                    }
                }
            } else {
                if (summaryRow) summaryRow->setVisibility(brls::Visibility::GONE);
                if (dlRow) dlRow->setVisibility(brls::Visibility::VISIBLE);

                // Download bar
                if (dlRow) {
                    auto* dlBarBg = dynamic_cast<brls::Box*>(dlRow->getChildren()[1]);
                    auto* dlBar = dlBarBg ? dynamic_cast<brls::Box*>(dlBarBg->getChildren()[0]) : nullptr;
                    auto* dlTxt = dynamic_cast<brls::Label*>(dlRow->getChildren()[2]);

                    float dl_prog = std::clamp(itm.progress, 0.0f, 1.0f);
                    if (dlBar) dlBar->setWidth(dl_prog * 110.0f);

                    char buf[64];
                    if (itm_dl_t > 0) {
                        std::snprintf(buf, sizeof(buf), "%d%% (%s)", static_cast<int>(dl_prog * 100.0f), formatProgressBytes(itm_dl_w, itm_dl_t).c_str());
                    } else {
                        std::snprintf(buf, sizeof(buf), "%d%% (%s)", static_cast<int>(dl_prog * 100.0f), formatBytes(itm_dl_w).c_str());
                    }
                    if (dlTxt) {
                        dlTxt->setText(buf);
                        dlTxt->setTextColor(secondaryText);
                    }
                }

                // Install bar
                if (instRow) {
                    bool canInstall = (itm.state == download::DownloadState::StreamPreparing ||
                                       itm.state == download::DownloadState::StreamInstalling ||
                                       itm.state == download::DownloadState::Installing ||
                                       (itm.is_port_auto_install && itm.install_total > 0));
                    instRow->setVisibility(canInstall ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

                    if (canInstall) {
                        auto* instBarBg = dynamic_cast<brls::Box*>(instRow->getChildren()[1]);
                        auto* instBar = instBarBg ? dynamic_cast<brls::Box*>(instBarBg->getChildren()[0]) : nullptr;
                        auto* instTxt = dynamic_cast<brls::Label*>(instRow->getChildren()[2]);

                        float inst_prog = std::clamp(itm.install_progress, 0.0f, 1.0f);
                        if (instBar) instBar->setWidth(inst_prog * 110.0f);

                        char buf[64];
                        if (itm_inst_t > 0) {
                            std::snprintf(buf, sizeof(buf), "%d%% (%s)", static_cast<int>(inst_prog * 100.0f), formatProgressBytes(itm_inst_w, itm_inst_t).c_str());
                        } else {
                            std::snprintf(buf, sizeof(buf), "%d%% (%s)", static_cast<int>(inst_prog * 100.0f), formatBytes(itm_inst_w).c_str());
                        }
                        if (instTxt) {
                            instTxt->setText(buf);
                            instTxt->setTextColor(secondaryText);
                        }
                    }
                }
            }

            // 2. Right Status Col
            auto* compStatusLabel = dynamic_cast<brls::Label*>(statusCol->getChildren()[0]);
            if (compStatusLabel) {
                std::string stStr = getStatusString(itm.state);
                compStatusLabel->setText(stStr);
                compStatusLabel->setTextColor(getDownloadStateColor(itm.state, isLight));
            }

            auto* compTimeLabel = dynamic_cast<brls::Label*>(statusCol->getChildren()[1]);
            if (compTimeLabel) {
                int64_t itemSec = getItemElapsedSeconds(itm);
                if (itemSec > 0 && itm.state != download::DownloadState::Queued) {
                    compTimeLabel->setText(formatElapsedSeconds(itemSec));
                    compTimeLabel->setTextColor(secondaryText);
                    compTimeLabel->setVisibility(brls::Visibility::VISIBLE);
                } else {
                    compTimeLabel->setText("");
                    compTimeLabel->setVisibility(brls::Visibility::GONE);
                }
            }
        }
    } else {
        cell->componentsBox->setVisibility(brls::Visibility::GONE);
    }

    // Configure Gamepad Controls on Cell Focus
    cell->clearRegisteredActions();

    if (group.state == download::DownloadState::Completed) {
        cell->registerAction("app/downloads/action_delete"_i18n, brls::ControllerButton::BUTTON_Y, [group](brls::View* view) {
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
            const auto& q = ui::DownloadManager::instance().getImpl().queue();
            for (size_t qidx : group.item_indices) {
                if (qidx < q.size()) {
                    ui::DownloadManager::instance().deleteDownload(q[qidx].topic_id);
                }
            }
            return true;
        });

        if (group.total_count > 1) {
            cell->registerAction(group.is_expanded ? "Свернуть" : "Развернуть", brls::ControllerButton::BUTTON_A, [this, row](brls::View* view) {
                if (row >= 0 && static_cast<size_t>(row) < groups_.size()) {
                    groups_[row].has_manual_toggle = true;
                    groups_[row].is_expanded = !groups_[row].is_expanded;
                    recycler->reloadData();
                }
                return true;
            }, false, false, brls::SOUND_CLICK);
        } else if (!group.item_indices.empty()) {
            size_t qidx = group.item_indices.front();
            if (qidx < queue.size() && isFileDownloadItem(queue[qidx])) {
                cell->registerAction("app/downloads/action_open_folder"_i18n, brls::ControllerButton::BUTTON_A, [item = queue[qidx]](brls::View* view) {
                    openDownloadsFolderForItem(item);
                    return true;
                }, false, false, brls::SOUND_CLICK);
            }
        }
    } else if (group.state == download::DownloadState::Failed || group.failed_item_idx >= 0) {
        // Hybrid Error Handling: [A] retries the specific failed item
        cell->registerAction("app/downloads/action_retry"_i18n, brls::ControllerButton::BUTTON_A, [group](brls::View* view) {
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
            const auto& q = ui::DownloadManager::instance().getImpl().queue();
            if (group.failed_item_idx >= 0 && static_cast<size_t>(group.failed_item_idx) < q.size()) {
                ui::DownloadManager::instance().retryDownload(q[group.failed_item_idx].topic_id);
            }
            return true;
        }, false, false, brls::SOUND_CLICK);

        cell->registerAction("app/downloads/action_delete"_i18n, brls::ControllerButton::BUTTON_Y, [group](brls::View* view) {
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
            const auto& q = ui::DownloadManager::instance().getImpl().queue();
            if (group.failed_item_idx >= 0 && static_cast<size_t>(group.failed_item_idx) < q.size()) {
                ui::DownloadManager::instance().deleteDownload(q[group.failed_item_idx].topic_id);
            }
            return true;
        });

        if (group.total_count > 1) {
            cell->registerAction(group.is_expanded ? "Свернуть" : "Развернуть", brls::ControllerButton::BUTTON_X, [this, row](brls::View* view) {
                if (row >= 0 && static_cast<size_t>(row) < groups_.size()) {
                    groups_[row].has_manual_toggle = true;
                    groups_[row].is_expanded = !groups_[row].is_expanded;
                    recycler->reloadData();
                }
                return true;
            }, false, false, brls::SOUND_CLICK);
        }
    } else {
        bool isPaused = (group.state == download::DownloadState::Paused);
        cell->registerAction(isPaused ? "app/downloads/action_resume"_i18n : "app/downloads/action_pause"_i18n,
                             brls::ControllerButton::BUTTON_X, [group, isPaused](brls::View* view) {
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
            const auto& q = ui::DownloadManager::instance().getImpl().queue();
            for (size_t qidx : group.item_indices) {
                if (qidx < q.size()) {
                    if (isPaused) {
                        ui::DownloadManager::instance().resumeDownload(q[qidx].topic_id);
                    } else {
                        ui::DownloadManager::instance().pauseDownload(q[qidx].topic_id);
                    }
                }
            }
            return true;
        });

        cell->registerAction("app/downloads/action_cancel"_i18n, brls::ControllerButton::BUTTON_Y, [group](brls::View* view) {
            std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
            const auto& q = ui::DownloadManager::instance().getImpl().queue();
            for (size_t qidx : group.item_indices) {
                if (qidx < q.size()) {
                    ui::DownloadManager::instance().cancelDownload(q[qidx].topic_id);
                }
            }
            return true;
        });

        if (group.total_count > 1) {
            cell->registerAction(group.is_expanded ? "Свернуть" : "Развернуть", brls::ControllerButton::BUTTON_A, [this, row](brls::View* view) {
                if (row >= 0 && static_cast<size_t>(row) < groups_.size()) {
                    groups_[row].has_manual_toggle = true;
                    groups_[row].is_expanded = !groups_[row].is_expanded;
                    recycler->reloadData();
                }
                return true;
            }, false, false, brls::SOUND_CLICK);
        }
    }

    // Swarm inspector action
    if (!group.item_indices.empty()) {
        size_t inspect_idx = (group.active_item_idx >= 0) ? static_cast<size_t>(group.active_item_idx) : group.item_indices.front();
        if (inspect_idx < queue.size()) {
            cell->registerAction("", brls::ControllerButton::BUTTON_LB, [item = queue[inspect_idx]](brls::View* view) {
                showPeerInspector(item);
                return true;
            }, true /* hidden */, false, brls::SOUND_CLICK);
        }
    }

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
    cell->loadedCoverUrl.clear();

    std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
    const auto& queue = ui::DownloadManager::instance().getImpl().queue();
    if (row >= 0 && static_cast<size_t>(row) < parent_->groups_.size()) {
        parent_->updateCell(cell, parent_->groups_[row], queue, row);
    }

    if (!cell->focusSubscribed) {
        cell->focusSubscribed = true;
        cell->getFocusEvent()->subscribe([this, cell](bool focused) {
            if (focused) {
                parent_->focusedRow_ = cell->getIndexPath().row;
            }
        });
    }

    return cell;
}

void DownloadsView::DownloadsDataSource::didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    int row = index.row;
    if (row < 0 || static_cast<size_t>(row) >= parent_->groups_.size()) return;
    auto& group = parent_->groups_[row];
    if (group.state == download::DownloadState::Failed && group.failed_item_idx >= 0) {
        std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
        const auto& q = ui::DownloadManager::instance().getImpl().queue();
        if (static_cast<size_t>(group.failed_item_idx) < q.size()) {
            ui::DownloadManager::instance().retryDownload(q[group.failed_item_idx].topic_id);
        }
        return;
    }
    if (group.total_count > 1) {
        group.has_manual_toggle = true;
        group.is_expanded = !group.is_expanded;
        recycler->reloadData();
    } else if (!group.item_indices.empty()) {
        std::lock_guard<std::recursive_mutex> lock(ui::DownloadManager::instance().getImpl().queueMutex());
        const auto& q = ui::DownloadManager::instance().getImpl().queue();
        size_t qidx = group.item_indices.front();
        if (qidx < q.size()) {
            const auto& item = q[qidx];
            if (item.state == download::DownloadState::Completed && isFileDownloadItem(item)) {
                openDownloadsFolderForItem(item);
            } else if (item.state == download::DownloadState::Failed || item.state == download::DownloadState::Cancelled) {
                ui::DownloadManager::instance().retryDownload(item.topic_id);
            }
        }
    }
}

} // namespace ui
