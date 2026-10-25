#include "PortInstallFlow.hpp"

#include <borealis.hpp>

#include <map>
#include <memory>
#include <set>

#include "DownloadUiManager.hpp"
#include "DownloadsView.hpp"
#include "../config/config.h"
#include "../installer/PortInstallPlanner.hpp"
#include "../installer/installed_ports.h"
#include "../utils/log.h"
#include "../utils/switch_utils.h"
#include "../datasource/custom_engine_client.h"

#include <atomic>
#include <functional>
#include <chrono>
#include <thread>

using namespace brls::literals;

namespace ui {

namespace {

std::string formatSize(unsigned long long bytes) {
    char buf[32];
    if (bytes >= 1024ULL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu KB", bytes / 1024);
    }
    return buf;
}

// "sdmc:/switch/codmw/game/a.pak" -> "sdmc:/switch/codmw/"
std::string topFolder(const std::string& dest) {
    if (dest.rfind("sdmc:/", 0) != 0) return "";
    std::string rest = dest.substr(6);
    size_t a = rest.find('/');
    if (a == std::string::npos) return "sdmc:/";
    if (rest.compare(0, a, "switch") == 0 || rest.compare(0, a, "atmosphere") == 0) {
        size_t b = rest.find('/', a + 1);
        if (b == std::string::npos) return "sdmc:/" + rest.substr(0, a + 1);
        return "sdmc:/" + rest.substr(0, b + 1);
    }
    return "sdmc:/" + rest.substr(0, a + 1);
}

std::string variantTitle(const installer::PortVariant& v) {
    if (v.title_key.empty()) return v.title;
    return v.title_arg.empty() ? brls::getStr(v.title_key) : brls::getStr(v.title_key, v.title_arg);
}

// Dialog body: one wrapping label per line. brls::Dialog(text) draws a
// multi-line text as one line that runs past the dialog edges.
struct TextLine {
    std::string text;
    float size = 20.0f;
    bool accent = false;
};

brls::Box* makeTextBox(const std::vector<TextLine>& lines) {
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setWidth(640.0f);
    box->setPadding(30, 36, 24, 36);
    for (const auto& l : lines) {
        auto* label = new brls::Label();
        label->setAutoAnimate(false);
        label->setSingleLine(false);
        label->setIsWrapping(true);
        label->setFontSize(l.size);
        label->setText(l.text);
        label->setMarginBottom(l.size > 20.0f ? 14.0f : 6.0f);
        if (l.accent) label->setTextColor(brls::Application::getTheme()["brls/accent"]);
        box->addView(label);
    }
    return box;
}

void showPreview(const Game& game, std::shared_ptr<installer::PortInstallPlan> plan, int variant) {
    const auto& v = plan->variants[static_cast<size_t>(variant)];

    std::set<std::string> folders;
    for (int idx : plan->getFilesForVariant(variant)) {
        auto it = plan->file_destinations.find(idx);
        if (it == plan->file_destinations.end()) continue;
        if (it->second.rfind("sdmc:/", 0) != 0) continue;   // temporary NSP
        bool isArchive = false;
        for (const auto& a : plan->archives_to_extract) {
            if (a.file_index == idx) isArchive = true;
        }
        folders.insert(isArchive ? it->second : topFolder(it->second));
    }
    std::vector<TextLine> lines;
    lines.push_back({cleanTitle(game.title), 24.0f});
    lines.push_back({brls::getStr("app/ports/preview_version", variantTitle(v))});
    lines.push_back({brls::getStr("app/ports/preview_download", formatSize(static_cast<unsigned long long>(v.total_size)))});
    int64_t sdFree = -1;
    util::getStorageFreeSpace(1, sdFree);
    if (sdFree >= 0) {
        lines.push_back({brls::getStr("app/ports/preview_free", formatSize(static_cast<unsigned long long>(sdFree)))});
    }
    // Archives: their unpacked size is known only from the listing read at install time.
    if (!plan->getArchivesForVariant(variant).empty()) lines.push_back({"app/ports/preview_space_note"_i18n, 17.0f});
    if (!folders.empty()) {
        lines.push_back({"app/ports/preview_folders"_i18n});
        size_t shown = 0;
        for (const auto& f : folders) {
            if (f.empty()) continue;
            if (shown++ == 5) {
                lines.push_back({"   …", 17.0f});
                break;
            }
            lines.push_back({"   " + f, 17.0f});
        }
    }
    const size_t forwarders = plan->forwardersForVariant(variant).size();
    lines.push_back({forwarders == 0 ? "app/ports/preview_no_forwarder"_i18n
                     : forwarders == 1 ? "app/ports/preview_forwarder"_i18n
                                       : brls::getStr("app/ports/preview_forwarders", std::to_string(forwarders))});
    const bool hasExtras = !plan->optional_file_indices.empty();
    if (hasExtras) {
        lines.push_back({brls::getStr("app/ports/preview_extras",
                                      formatSize(static_cast<unsigned long long>(plan->optional_size))), 18.0f});
    }
    if (!plan->recipe_id.empty()) lines.push_back({"app/ports/preview_recipe"_i18n, 17.0f, true});

    auto* dialog = new brls::Dialog(makeTextBox(lines));
    dialog->setCancelable(true);
    Game gameCopy = game;
    dialog->addButton("app/ports/install_btn"_i18n, [gameCopy, plan, variant]() {
        ui::DownloadManager::instance().addPortInstall(gameCopy, plan, variant, false);
        brls::sync([]() { brls::Application::pushActivity(new ui::DownloadsView()); });
    });
    if (hasExtras) {
        dialog->addButton(brls::getStr("app/ports/install_with_extras_btn",
                                       formatSize(static_cast<unsigned long long>(plan->optional_size))),
                          [gameCopy, plan, variant]() {
            ui::DownloadManager::instance().addPortInstall(gameCopy, plan, variant, true);
            brls::sync([]() { brls::Application::pushActivity(new ui::DownloadsView()); });
        });
    }
    // The whole release as files in downloads/, nothing installed.
    dialog->addButton("app/ports/download_files_btn"_i18n, [gameCopy]() {
        ui::DownloadManager::instance().addDownload(gameCopy, {}, -1, "");
        brls::sync([]() { brls::Application::pushActivity(new ui::DownloadsView()); });
    });
    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

void chooseVariant(const Game& game, std::shared_ptr<installer::PortInstallPlan> plan) {
    if (plan->variants.size() <= 1) {
        showPreview(game, plan, 0);
        return;
    }
    std::vector<std::string> labels;
    for (size_t i = 0; i < plan->variants.size(); ++i) {
        const auto& v = plan->variants[i];
        std::string label = variantTitle(v) + "  •  " + formatSize(static_cast<unsigned long long>(v.total_size));
        if (static_cast<int>(i) == plan->default_variant_index) label = "★ " + label;
        labels.push_back(label);
    }
    auto chosen = std::make_shared<int>(-1);
    Game gameCopy = game;
    // Dropdown calls dismissCb on B as well, so the pick comes from the selection callback.
    auto* dropdown = new brls::Dropdown(
        "app/detail/choose_version_title"_i18n, labels,
        [chosen](int sel) { *chosen = sel; },
        plan->default_variant_index,
        [gameCopy, plan, chosen](int) {
            if (*chosen < 0 || *chosen >= static_cast<int>(plan->variants.size())) return;
            const int v = *chosen;
            brls::sync([gameCopy, plan, v]() { showPreview(gameCopy, plan, v); });
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

} // namespace

namespace {

// One analysis at a time: a second press while the list is being fetched does nothing.
std::atomic<bool> g_probeRunning{false};

struct ProbeUi {
    std::atomic<bool> running{true};
    std::atomic<bool> cancelled{false};
    brls::Dialog* dialog = nullptr;   // valid while running && !cancelled
    brls::Label* status = nullptr;
};

void showMessage(const std::string& text, const std::string& actionLabel = "",
                 std::function<void()> action = nullptr) {
    std::vector<TextLine> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string part = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!part.empty()) lines.push_back({part});
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    auto* d = new brls::Dialog(makeTextBox(lines));
    d->setCancelable(true);
    if (action) {
        d->addButton(actionLabel, action);
        d->addButton("app/common/cancel"_i18n, []() {});
    } else {
        d->addButton("app/common/ok"_i18n, []() {});
    }
    d->open();
}

} // namespace

void startPortInstallFlow(const Game& game) {
    if (ui::DownloadManager::instance().hasActiveDownload(game.topic_id, game.magnet, -1)) {
        brls::Application::notify("app/downloads/already_in_queue"_i18n);
        return;
    }
    if (g_probeRunning.exchange(true)) return;

    // Modal "analyzing" window: spinner, live swarm status, Cancel.
    auto ui = std::make_shared<ProbeUi>();
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setWidth(640.0f);
    box->setPadding(40, 40, 30, 40);
    auto* spinner = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
    spinner->setWidth(64);
    spinner->setHeight(64);
    spinner->setMarginBottom(24);
    box->addView(spinner);
    auto* title = new brls::Label();
    title->setText("app/ports/probe_title"_i18n);
    title->setFontSize(24);
    title->setMarginBottom(12);
    box->addView(title);
    auto* hint = new brls::Label();
    hint->setAutoAnimate(false);
    hint->setSingleLine(false);
    hint->setIsWrapping(true);
    hint->setText("app/ports/probe_wait"_i18n);
    hint->setFontSize(18);
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    hint->setMarginBottom(12);
    box->addView(hint);
    ui->status = new brls::Label();
    ui->status->setText("app/fileselect/probing"_i18n);
    ui->status->setFontSize(16);
    ui->status->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    box->addView(ui->status);

    ui->dialog = new brls::Dialog(box);
    ui->dialog->setCancelable(false);
    ui->dialog->addButton("app/common/cancel"_i18n, [ui]() {
        ui->cancelled.store(true);
        datasource::CustomEngineClient::instance().cancelProbe();
        util::logLine("PortInstallFlow: analysis cancelled by the user");
    });
    ui->dialog->open();

    // Live status of the metadata search (same text as the file-select screen).
    brls::async([ui]() {
        while (ui->running.load() && !ui->cancelled.load() && !g_appExiting.load()) {
            for (int i = 0; i < 10 && ui->running.load() && !ui->cancelled.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!ui->running.load() || ui->cancelled.load()) break;
            auto s = datasource::CustomEngineClient::instance().probeStatus();
            brls::sync([ui, s]() {
                if (!ui->running.load() || ui->cancelled.load() || !ui->status) return;
                std::string text;
                if (s.active && s.meta_peers_total > 0) {
                    text = brls::getStr("app/fileselect/meta_peers", std::to_string(s.meta_peers_tried),
                                        std::to_string(s.meta_peers_total));
                } else if (s.active) {
                    text = brls::getStr("app/fileselect/peer_search", s.phase);
                } else {
                    text = brls::getStr("app/fileselect/swarm_stats", std::to_string(s.seeds),
                                        std::to_string(s.peers), std::to_string(s.dht_nodes));
                }
                ui->status->setText(text);
            });
        }
    });

    Game gameCopy = game;
    brls::async([gameCopy, ui]() {
        std::vector<torrent::TorrentFileInfo> files;
        std::string err;
        bool ok = ui::DownloadManager::instance().getImpl().probeTorrentFiles(gameCopy.magnet, files, &err);
        std::shared_ptr<installer::PortInstallPlan> plan;
        if (ok && !files.empty() && !ui->cancelled.load()) {
            // "auto" in settings means the system language the UI runs in.
            std::string lang = config::ConfigManager::instance().getLanguage();
            if (lang.empty() || lang == "auto") lang = brls::Application::getLocale();
            plan = std::make_shared<installer::PortInstallPlan>(
                installer::PortInstallPlanner::instance().plan(gameCopy.topic_id, gameCopy.title, files, lang));
            util::logLine("PortInstallFlow: " + plan->describe(files, 40));
        } else if (!ui->cancelled.load()) {
            util::logLine("PortInstallFlow: probe failed for " + gameCopy.title + ": " + err);
        }
        brls::sync([gameCopy, ok, plan, ui]() {
            ui->running.store(false);
            g_probeRunning.store(false);
            if (ui->cancelled.load()) return;   // the dialog is already closed
            ui->status = nullptr;
            // Next step only after the waiting window is gone.
            ui->dialog->close([gameCopy, ok, plan]() {
                if (!ok || !plan) {
                    showMessage("app/detail/auto_install_probe_failed"_i18n + "\n\n" + "app/ports/probe_failed_hint"_i18n,
                                "app/ports/retry_btn"_i18n, [gameCopy]() {
                        brls::sync([gameCopy]() { startPortInstallFlow(gameCopy); });
                    });
                    return;
                }
                if (!plan->is_port || plan->variants.empty()) {
                    // Not something the installer understands: offer the plain download.
                    showMessage("app/detail/auto_install_not_port"_i18n, "app/ports/download_files_btn"_i18n, [gameCopy]() {
                        ui::DownloadManager::instance().addDownload(gameCopy, {}, -1, "");
                        brls::sync([]() { brls::Application::pushActivity(new ui::DownloadsView()); });
                    });
                    return;
                }
                chooseVariant(gameCopy, plan);
            });
        });
    });
}

bool isPortInstalled(const Game& game) {
    return installer::InstalledPorts::instance().isInstalled(game.topic_id, game.title);
}

void confirmPortUninstall(const Game& game) {
    const std::string key = installer::InstalledPorts::keyFor(game.topic_id, game.title);
    installer::InstalledPort port;
    if (!installer::InstalledPorts::instance().load(key, port)) {
        brls::Application::notify("app/ports/not_installed"_i18n);
        return;
    }
    auto* dialog = new brls::Dialog(makeTextBox({{brls::getStr("app/ports/uninstall_confirm", cleanTitle(game.title),
                                                               std::to_string(port.files.size()))}}));
    dialog->setCancelable(true);
    dialog->addButton("app/ports/uninstall_btn"_i18n, [key]() {
        brls::async([key]() {
            size_t removed = 0;
            std::string err;
            bool ok = installer::InstalledPorts::instance().uninstall(key, removed, err);
            brls::sync([ok, removed, err]() {
                brls::Application::notify(ok ? brls::getStr("app/ports/uninstall_done", std::to_string(removed))
                                             : err);
            });
        });
    });
    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

} // namespace ui
