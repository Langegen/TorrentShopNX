#include "details.h"

#include <switch.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../datasource/data_source_manager.h"
#include "../datasource/internal_torrent_engine.h"
#include "../catalog/favorites_manager.h"
#include "input.h"
#include "downloads.h"
#include "console_layout.h"

namespace ui {

static bool endsWithIcase(const std::string& s, const std::string& ext) {
    if (s.size() < ext.size()) return false;
    size_t off = s.size() - ext.size();
    for (size_t i = 0; i < ext.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(s[off + i]);
        unsigned char b = static_cast<unsigned char>(ext[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

static bool isInstallableTorrentFile(const std::string& name) {
    return endsWithIcase(name, ".nsp") ||
           endsWithIcase(name, ".nsz") ||
           endsWithIcase(name, ".xci") ||
           endsWithIcase(name, ".pfs0");
}

static int installableFilePriority(const std::string& name) {
    if (endsWithIcase(name, ".nsp")) return 4;
    if (endsWithIcase(name, ".nsz")) return 3;
    if (endsWithIcase(name, ".xci")) return 2;
    if (endsWithIcase(name, ".pfs0")) return 1;
    return 0;
}

static int chooseDefaultInstallableIndex(const std::vector<torrent::TorrentFileInfo>& files) {
    int best_index = -1;
    int best_priority = -1;
    unsigned long long best_size = 0;

    for (size_t i = 0; i < files.size(); ++i) {
        const int priority = installableFilePriority(files[i].name);
        if (priority <= 0) continue;

        if (best_index < 0 ||
            priority > best_priority ||
            (priority == best_priority && files[i].size > best_size)) {
            best_index = static_cast<int>(i);
            best_priority = priority;
            best_size = files[i].size;
        }
    }

    return best_index;
}

static const char* formatSize(unsigned long long bytes, char* out, size_t out_len) {
    const double b = static_cast<double>(bytes);
    if (b >= 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(out, out_len, "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    } else if (b >= 1024.0 * 1024.0) {
        std::snprintf(out, out_len, "%.2f MB", b / (1024.0 * 1024.0));
    } else if (b >= 1024.0) {
        std::snprintf(out, out_len, "%.2f KB", b / 1024.0);
    } else {
        std::snprintf(out, out_len, "%llu B", bytes);
    }
    return out;
}

static const char* torrentStateName(int state) {
    switch (state) {
        case 0: return "checking_resume";
        case 1: return "downloading_metadata";
        case 2: return "downloading";
        case 3: return "finished";
        case 4: return "seeding";
        case 5: return "allocating";
        case 6: return "checking_files";
        default: return "unknown";
    }
}

static void summarizeSelection(const std::vector<torrent::TorrentFileInfo>& files,
                               const std::vector<bool>& selected,
                               int& out_count,
                               unsigned long long& out_size) {
    out_count = 0;
    out_size = 0;
    for (size_t i = 0; i < files.size() && i < selected.size(); ++i) {
        if (!selected[i]) continue;
        ++out_count;
        out_size += files[i].size;
    }
}

static void showInfoWait(const std::string& title, const std::string& body, download::DownloadManager& downloads) {
    while (appletMainLoop()) {
        downloads.trackProgress();
        layout::useFullScreenWindow();
        consoleClear();
        printf("%s\n\n", layout::encodeForConsole(title).c_str());
        const auto lines = layout::wrapTextUtf8(body, static_cast<size_t>(std::max(20, layout::printableWidth(1))));
        for (const auto& line : lines) {
            printf("%s\n", layout::encodeForConsole(line).c_str());
        }
        printf("\n");
        printf("B: Back\n");
        if (inputDown() & HidNpadButton_B) return;
        consoleUpdate(NULL);
    }
}

static bool loadTorrentFilesWithProgress(const catalog::CatalogEntry& entry,
                                         download::DownloadManager& downloads,
                                         std::vector<torrent::TorrentFileInfo>& out_files,
                                         std::string& out_error) {
    out_files.clear();
    out_error.clear();

    const bool local_mode =
        downloads.dataSourceManager().mode() == datasource::DataSourceMode::LocalClient;
    if (!local_mode) {
        return downloads.probeTorrentFiles(entry.magnet, out_files, &out_error);
    }

    std::atomic<bool> done{false};
    bool success = false;
    std::thread worker([&]() {
        success = downloads.probeTorrentFiles(entry.magnet, out_files, &out_error);
        done.store(true);
    });

    bool cancelled = false;
    while (appletMainLoop() && !done.load()) {
        layout::useFullScreenWindow();
        consoleClear();

        const auto status = datasource::InternalTorrentEngine::instance().probeStatus();
        const std::string phase = status.phase.empty() ? "starting torrent" : status.phase;
        const int width = std::max(24, layout::printableWidth());

        printf("Loading torrent files...\n\n");
        printf("Status: %s\n", layout::encodeForConsole(
            layout::ellipsizeUtf8(phase, static_cast<size_t>(width - 8))).c_str());
        printf("Metadata: %s\n", status.has_metadata ? "ready" : "waiting");
        printf("State: %s\n", torrentStateName(status.torrent_state));
        printf("Listen: %s", status.session_listening ? "yes" : "no");
        if (status.listen_port > 0) {
            printf(":%d", status.listen_port);
        }
        printf("\n");
        printf("Peers: %d | Seeds: %d\n", status.peers, status.seeds);
        printf("Known: %d | Candidates: %d\n", status.known_peers, status.connect_candidates);
        printf("DHT nodes: %d\n", status.dht_nodes);
        if (!status.hash.empty()) {
            printf("Hash: %s\n", layout::encodeForConsole(
                layout::ellipsizeUtf8(status.hash, static_cast<size_t>(width - 6))).c_str());
        }
        if (!status.detail.empty()) {
            printf("Detail: %s\n", layout::encodeForConsole(
                layout::ellipsizeUtf8(status.detail, static_cast<size_t>(width - 8))).c_str());
        }
        printf("\n");
        printf("B: Cancel\n");
        consoleUpdate(NULL);

        if (inputDown() & HidNpadButton_B) {
            datasource::InternalTorrentEngine::instance().cancelProbe();
            cancelled = true;
            break;
        }

#ifdef __SWITCH__
        svcSleepThread(100000000LL);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    }

    if (cancelled && !done.load()) {
        datasource::InternalTorrentEngine::instance().cancelProbe();
    }
    if (worker.joinable()) {
        worker.join();
    }

    if (cancelled) {
        out_files.clear();
        out_error = "Loading torrent files cancelled.";
        return false;
    }

    return success;
}

static bool showTorrentFileSelector(const catalog::CatalogEntry& entry, download::DownloadManager& downloads) {
    layout::useFullScreenWindow();
    consoleClear();
    printf("Loading torrent files...\n");
    consoleUpdate(NULL);

    std::vector<torrent::TorrentFileInfo> files;
    std::string err;
    if (!loadTorrentFilesWithProgress(entry, downloads, files, err) || files.empty()) {
        showInfoWait("Download", err.empty() ? "Failed to load torrent files." : err, downloads);
        return false;
    }

    std::vector<bool> selected(files.size(), false);
    const int default_index = chooseDefaultInstallableIndex(files);
    if (default_index >= 0) {
        selected[default_index] = true;
    }

    int cur = 0;
    std::string hint;

    while (appletMainLoop()) {
        downloads.trackProgress();
        layout::useFullScreenWindow();
        consoleClear();

        const int width = layout::printableWidth();
        const int height = layout::consoleHeight();
        const int header_lines = 5;
        const int footer_lines = hint.empty() ? 3 : 4;
        const int item_height = 4;
        const int page_size = std::max(1, (height - header_lines - footer_lines) / item_height);
        const int total_items = static_cast<int>(files.size());
        cur = layout::clampIndex(cur, total_items);
        const int current_page = cur / page_size;
        const int start = current_page * page_size;
        const int end = std::min(start + page_size, total_items);
        const int total_pages = layout::pageCount(total_items, page_size);

        printf("Select Files To Install\n");
        printf("%s\n", layout::encodeForConsole(
            layout::ellipsizeUtf8(entry.title, static_cast<size_t>(std::max(16, width - 1)))).c_str());

        int selected_count = 0;
        unsigned long long selected_size = 0;
        summarizeSelection(files, selected, selected_count, selected_size);
        char selected_size_buf[32];
        printf("Selected: %d | Size: %s\n",
               selected_count,
               formatSize(selected_size, selected_size_buf, sizeof(selected_size_buf)));
        printf("%s\n", layout::separatorLine('=').c_str());

        for (int i = start; i < end; ++i) {
            const auto& f = files[i];
            const bool selectable = isInstallableTorrentFile(f.name);
            char size_buf[32];
            const char* mark = selected[i] ? "[x]" : "[ ]";
            const auto name_lines = layout::fixedTextBlock(
                f.name,
                static_cast<size_t>(std::max(8, width - 8)),
                2);
            std::string meta = formatSize(f.size, size_buf, sizeof(size_buf));
            if (!selectable) meta += " | skip";
            const bool has_second_name = !name_lines[1].empty();
            printf("%s %s %s\n",
                   i == cur ? ">" : " ",
                   mark,
                   layout::encodeForConsole(name_lines[0]).c_str());
            if (has_second_name) {
                printf("      %s\n", layout::encodeForConsole(name_lines[1]).c_str());
                printf("      %s\n", layout::encodeForConsole(
                    layout::ellipsizeUtf8(meta, static_cast<size_t>(std::max(8, width - 8)))).c_str());
            } else {
                printf("      %s\n", layout::encodeForConsole(
                    layout::ellipsizeUtf8(meta, static_cast<size_t>(std::max(8, width - 8)))).c_str());
                printf("      \n");
            }
            printf("\n");
        }

        printf("%s\n", layout::separatorLine('=').c_str());
        if (!hint.empty()) {
            printf("%s\n", layout::encodeForConsole(
                layout::ellipsizeUtf8(hint, static_cast<size_t>(std::max(12, width - 1)))).c_str());
        }
        printf("A: Toggle  X: Select All  Y: Clear  +: Queue  B: Back\n");
        printf("Page %d/%d\n", current_page + 1, total_pages);
        consoleUpdate(NULL);

        u64 kDown = inputDown();
        u64 kNav = kDown | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
        if (kNav & HidNpadButton_Down) {
            if (!files.empty()) cur = (cur + 1) % files.size();
        }
        if (kNav & HidNpadButton_Up) {
            if (!files.empty()) cur = (cur - 1 + (int)files.size()) % files.size();
        }
        if ((kDown & HidNpadButton_A) && !files.empty()) {
            if (isInstallableTorrentFile(files[cur].name)) {
                selected[cur] = !selected[cur];
                hint.clear();
            } else {
                hint = "This file type is not installable.";
            }
        }
        if (kDown & HidNpadButton_X) {
            for (size_t i = 0; i < files.size(); ++i) {
                selected[i] = isInstallableTorrentFile(files[i].name);
            }
            hint.clear();
        }
        if (kDown & HidNpadButton_Y) {
            std::fill(selected.begin(), selected.end(), false);
            hint.clear();
        }
        if (kDown & HidNpadButton_Plus) {
            std::vector<size_t> chosen;
            chosen.reserve(files.size());
            for (size_t i = 0; i < files.size(); ++i) {
                if (selected[i]) chosen.push_back(i);
            }

            if (chosen.empty()) {
                hint = "Select at least one file.";
                continue;
            }

            size_t first_queue_index = static_cast<size_t>(-1);
            for (size_t k = 0; k < chosen.size(); ++k) {
                const auto& f = files[chosen[k]];
                std::string item_title = entry.title;
                if (chosen.size() > 1) {
                    item_title += " [" + f.name + "]";
                }
                size_t idx = downloads.addToQueue(item_title, entry.magnet, f.index, f.name);
                if (first_queue_index == static_cast<size_t>(-1)) {
                    first_queue_index = idx;
                }
            }

            if (first_queue_index != static_cast<size_t>(-1)) {
                downloads.startDownload(first_queue_index);
                return true;
            }

            hint = "Failed to queue selected files.";
        }
        if (kDown & HidNpadButton_B) return false;
    }

    return false;
}

void showEntryDetails(const catalog::CatalogEntry& entry, download::DownloadManager& downloads) {
    int selected = 0;

    while (appletMainLoop()) {
        downloads.trackProgress();
        layout::useFullScreenWindow();
        consoleClear();

        const int width = layout::printableWidth();
        const auto title_lines = layout::fixedTextBlock(entry.title, static_cast<size_t>(std::max(16, width)), 2);
        printf("%s\n", layout::encodeForConsole(title_lines[0]).c_str());
        if (!title_lines[1].empty()) {
            printf("%s\n", layout::encodeForConsole(title_lines[1]).c_str());
        }
        printf("\n");
        printf("Category: %s\n", layout::encodeForConsole(
            layout::ellipsizeUtf8(entry.category, static_cast<size_t>(std::max(12, width - 11)))).c_str());
        printf("Size: %s\n\n", layout::encodeForConsole(entry.size).c_str());

        const auto description_lines = layout::wrapTextUtf8(
            entry.description,
            static_cast<size_t>(std::max(20, width)),
            8);
        for (const auto& line : description_lines) {
            printf("%s\n", layout::encodeForConsole(line).c_str());
        }
        printf("\n");

        bool is_fav = catalog::FavoritesManager::instance().isFavorite(entry.title);
        const char* options[] = {"Download", "Queue", is_fav ? "Remove from Favorites" : "Add to Favorites", "Back"};

        for (int i = 0; i < 4; ++i) {
            if (i == selected) printf(" > %s\n", options[i]);
            else printf("   %s\n", options[i]);
        }

        u64 kDown = inputDown();
        u64 kNav = kDown | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
        if (kNav & HidNpadButton_Down) selected = (selected + 1) % 4;
        if (kNav & HidNpadButton_Up) selected = (selected - 1 + 4) % 4;
        if (kDown & HidNpadButton_A) {
            if (selected == 0) {
                if (showTorrentFileSelector(entry, downloads)) {
                    showDownloads(downloads);
                    return;
                }
            } else if (selected == 1) {
                downloads.addToQueue(entry.title, entry.magnet);
            } else if (selected == 2) {
                catalog::FavoritesManager::instance().toggleFavorite(entry);
            }
            if (selected == 3) return;
        }
        if (kDown & HidNpadButton_B) return;
        consoleUpdate(NULL);
    }
}

} // namespace ui
