#include "downloads.h"

#include <switch.h>
#include <algorithm>
#include <cstdio>
#include <numeric>

#include "input.h"
#include "console_layout.h"
#include "../utils/log.h"

namespace ui {

static const char* stateToString(download::DownloadState s) {
    switch (s) {
        case download::DownloadState::Queued:           return "Queued";
        case download::DownloadState::Downloading:      return "Downloading";
        case download::DownloadState::StreamPreparing:  return "Preparing Stream";
        case download::DownloadState::StreamInstalling: return "Downloading + Install";
        case download::DownloadState::Installing:       return "Installing";
        case download::DownloadState::Completed:        return "Completed";
        case download::DownloadState::Cancelled:        return "Cancelled";
        case download::DownloadState::Failed:           return "Error";
        default: return "???";
    }
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

static bool hasExt(const std::string& name, const std::string& ext) {
    if (name.size() < ext.size()) return false;
    auto pos = name.rfind(ext);
    return pos != std::string::npos && pos + ext.size() == name.size();
}

static void drawProgressBar(float pct, int width) {
    int filled = static_cast<int>(pct * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;

    printf("[");
    for (int i = 0; i < width; ++i) {
        if (i < filled) printf("=");
        else if (i == filled) printf(">");
        else printf(" ");
    }
    if (pct < 0.0001f) {
        printf("] %.6f%%", pct * 100.0f);
    } else if (pct < 0.001f) {
        printf("] %.5f%%", pct * 100.0f);
    } else if (pct < 0.01f) {
        printf("] %.4f%%", pct * 100.0f);
    } else if (pct < 0.1f) {
        printf("] %.2f%%", pct * 100.0f);
    } else {
        printf("] %.1f%%", pct * 100.0f);
    }
}

static int countActiveDownloads(const std::vector<download::DownloadItem>& queue) {
    int active = 0;
    for (const auto& item : queue) {
        if (item.state == download::DownloadState::Downloading ||
            item.state == download::DownloadState::StreamPreparing ||
            item.state == download::DownloadState::StreamInstalling ||
            item.state == download::DownloadState::Installing) {
            ++active;
        }
    }
    return active;
}

static std::string formatSpeed(float download_speed_kbps) {
    char buf[64] = {0};
    if (download_speed_kbps >= 1024.0f) {
        std::snprintf(buf, sizeof(buf), "%.2f MB/s", download_speed_kbps / 1024.0f);
    } else if (download_speed_kbps >= 1.0f) {
        std::snprintf(buf, sizeof(buf), "%.2f KB/s", download_speed_kbps);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f B/s", download_speed_kbps * 1024.0f);
    }
    return buf;
}

static bool isActiveState(download::DownloadState state) {
    return state == download::DownloadState::Downloading ||
           state == download::DownloadState::StreamPreparing ||
           state == download::DownloadState::StreamInstalling ||
           state == download::DownloadState::Installing;
}

static int displayPriority(download::DownloadState state) {
    switch (state) {
        case download::DownloadState::Downloading:
        case download::DownloadState::StreamPreparing:
        case download::DownloadState::StreamInstalling:
        case download::DownloadState::Installing:
            return 0;
        case download::DownloadState::Queued:
            return 1;
        case download::DownloadState::Failed:
            return 2;
        case download::DownloadState::Cancelled:
            return 3;
        case download::DownloadState::Completed:
            return 4;
        default:
            return 5;
    }
}

static std::vector<size_t> buildDisplayOrder(const std::vector<download::DownloadItem>& queue) {
    std::vector<size_t> order(queue.size());
    std::iota(order.begin(), order.end(), static_cast<size_t>(0));

    std::stable_sort(order.begin(), order.end(),
        [&queue](size_t a, size_t b) {
            const int pa = displayPriority(queue[a].state);
            const int pb = displayPriority(queue[b].state);
            if (pa != pb) return pa < pb;
            return a < b;
        });
    return order;
}

static void drawQueueItem(const download::DownloadItem& item, bool selected, int width) {
    const int title_width = std::max(10, width - 4);
    const int text_width = std::max(16, width - 10);
    const int bar_width = std::max(10, std::min(30, width - 20));

    const std::string encoded_title = layout::encodeForConsole(
        layout::ellipsizeUtf8(item.title, static_cast<size_t>(title_width)));
    printf("%s %s\n",
           selected ? ">" : " ",
           encoded_title.c_str());

    std::string state_line = std::string("  State : ") + stateToString(item.state);
    if (isActiveState(item.state)) {
        state_line += " | " + formatSpeed(item.download_speed_kbps);
        if (item.start_time.time_since_epoch().count() != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - item.start_time).count();
            int mins = static_cast<int>(elapsed / 60);
            int secs = static_cast<int>(elapsed % 60);
            char time_buf[32];
            std::snprintf(time_buf, sizeof(time_buf), " | %02d:%02d", mins, secs);
            state_line += time_buf;
        }
    }
    printf("%s\n", layout::encodeForConsole(
        layout::ellipsizeUtf8(state_line, static_cast<size_t>(std::max(20, width - 1)))).c_str());

    switch (item.state) {
        case download::DownloadState::StreamPreparing: {
            static int spin_frame = 0;
            spin_frame++;
            const char spin_char = "|/-\\"[(spin_frame / 4) % 4];
            printf("  Status: Connecting to peers %c\n", spin_char);
            printf("  \n");
            printf("  \n");
            break;
        }
        case download::DownloadState::Downloading:
        case download::DownloadState::StreamInstalling: {
            printf("  DL    : ");
            drawProgressBar(item.progress, bar_width);
            printf("\n");

            printf("  INST  : ");
            drawProgressBar(item.install_progress, bar_width);
            printf("\n");

            std::string footer;
            if (item.hybrid_installer) {
                footer = item.hybrid_installer->statusText();
            } else {
                footer = "Waiting for install stream...";
            }
            printf("  %s\n", layout::encodeForConsole(
                layout::ellipsizeUtf8(footer, static_cast<size_t>(text_width))).c_str());
            break;
        }
        case download::DownloadState::Installing:
            printf("  INST  : ");
            drawProgressBar(item.install_progress, bar_width);
            printf("\n");
            printf("  Status: Finalizing install\n");
            printf("  \n");
            break;
        case download::DownloadState::Completed:
            printf("  Result: OK\n");
            printf("  \n");
            printf("  \n");
            break;
        case download::DownloadState::Cancelled:
            printf("  Result: Cancelled\n");
            printf("  \n");
            printf("  \n");
            break;
        case download::DownloadState::Failed:
            printf("  Error : %s\n",
                   layout::encodeForConsole(
                       layout::ellipsizeUtf8(item.error_message, static_cast<size_t>(text_width))).c_str());
            printf("  \n");
            printf("  \n");
            break;
        default:
            printf("  \n");
            printf("  \n");
            printf("  \n");
            break;
    }
}

static void showFilePicker(download::DownloadManager& downloads, size_t index) {
    std::vector<torrent::TorrentFileInfo> files;
    if (!downloads.getTorrentFiles(index, files)) {
        layout::useFullScreenWindow();
        consoleClear();
        printf("File list is unavailable.\n\n");
        printf("Please wait for metadata to load.\n\n");
        printf("B: Back\n");
        while (appletMainLoop()) {
            if (inputDown() & HidNpadButton_B) return;
            consoleUpdate(NULL);
        }
        return;
    }

    int selected = 0;
    bool ext_nsp = true;
    bool ext_pfs0 = true;

    while (appletMainLoop()) {
        layout::useFullScreenWindow();
        consoleClear();

        const int width = layout::consoleWidth();
        const int height = layout::consoleHeight();
        const int header_lines = 5;
        const int footer_lines = 3;
        const int page_size = std::max(1, (height - header_lines - footer_lines) / 2);
        const int total_items = static_cast<int>(files.size());
        selected = layout::clampIndex(selected, total_items);
        const int current_page = selected / page_size;
        const int start = current_page * page_size;
        const int end = std::min(start + page_size, total_items);
        const int total_pages = layout::pageCount(total_items, page_size);

        printf("File Selection\n");

        unsigned long long total = 0;
        unsigned long long wanted = 0;
        for (const auto& f : files) {
            total += f.size;
            if (f.wanted) wanted += f.size;
        }
        char total_buf[32];
        char wanted_buf[32];
        printf("Total: %s | Selected: %s\n",
               formatSize(total, total_buf, sizeof(total_buf)),
               formatSize(wanted, wanted_buf, sizeof(wanted_buf)));
        printf("Filters: %s %s\n", ext_nsp ? ".nsp" : "-----", ext_pfs0 ? ".pfs0" : "------");
        printf("%s\n", layout::separatorLine('=').c_str());

        for (int i = start; i < end; ++i) {
            const auto& f = files[i];
            char size_buf[32];
            const char* mark = f.wanted ? "[x]" : "[ ]";
            const std::string encoded_name = layout::encodeForConsole(
                layout::ellipsizeUtf8(f.name, static_cast<size_t>(std::max(10, width - 10))));
            printf("%s %s %s\n",
                   i == selected ? ">" : " ",
                   mark,
                   encoded_name.c_str());
            printf("  %s\n", formatSize(f.size, size_buf, sizeof(size_buf)));
        }

        printf("%s\n", layout::separatorLine('=').c_str());
        printf("A: Toggle  X: By Filter  L/R: Filters  B: Back\n");
        printf("Page %d/%d\n", current_page + 1, total_pages);

        u64 kDown = inputDown();
        u64 kNav = kDown | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
        if (kNav & HidNpadButton_Down) {
            if (!files.empty()) selected = (selected + 1) % total_items;
        }
        if (kNav & HidNpadButton_Up) {
            if (!files.empty()) selected = (selected - 1 + total_items) % total_items;
        }
        if (kDown & HidNpadButton_L) ext_nsp = !ext_nsp;
        if (kDown & HidNpadButton_R) ext_pfs0 = !ext_pfs0;
        if (kDown & HidNpadButton_A) {
            if (!files.empty()) {
                auto& f = files[selected];
                bool new_wanted = !f.wanted;
                if (downloads.setFileWanted(index, f.index, new_wanted)) {
                    f.wanted = new_wanted;
                }
            }
        }
        if (kDown & HidNpadButton_X) {
            for (auto& f : files) {
                bool match = (ext_nsp && hasExt(f.name, ".nsp")) || (ext_pfs0 && hasExt(f.name, ".pfs0"));
                if (downloads.setFileWanted(index, f.index, match)) {
                    f.wanted = match;
                }
            }
        }
        if (kDown & HidNpadButton_B) return;
        consoleUpdate(NULL);
    }
}

void showDownloads(download::DownloadManager& downloads) {
    int selected = 0;

    while (appletMainLoop()) {
        downloads.trackProgress();
        layout::useFullScreenWindow();
        consoleClear();

        const auto& q = downloads.queue();
        const int total_items = static_cast<int>(q.size());
        if (q.empty()) {
            selected = 0;
        } else {
            selected = layout::clampIndex(selected, total_items);
        }

        auto order = buildDisplayOrder(q);
        if (!q.empty()) {
            auto it = std::find(order.begin(), order.end(), static_cast<size_t>(selected));
            if (it == order.end()) {
                selected = static_cast<int>(order.front());
                it = order.begin();
            }

            const int width = layout::consoleWidth();
            const int height = layout::consoleHeight();
            const int header_lines = 4;
            const int footer_lines = 3;
            const int item_lines = 6;
            const int page_size = std::max(1, (height - header_lines - footer_lines) / item_lines);
            const int selected_pos = static_cast<int>(std::distance(order.begin(), it));
            const int current_page = selected_pos / page_size;
            const int start = current_page * page_size;
            const int end = std::min(start + page_size, total_items);
            const int total_pages = layout::pageCount(total_items, page_size);

            printf("Downloads\n");
            printf("Source: %s\n", downloads.dataSourceManager().modeDescription().c_str());
            printf("Items: %d | Active: %d | Selected: %d/%d\n",
                   total_items,
                   countActiveDownloads(q),
                   selected_pos + 1,
                   total_items);
            printf("%s\n", layout::separatorLine('=').c_str());

            for (int display_pos = start; display_pos < end; ++display_pos) {
                const size_t queue_index = order[display_pos];
                drawQueueItem(q[queue_index], static_cast<int>(queue_index) == selected, width);
                printf("%s\n", layout::separatorLine('-').c_str());
            }

            printf("A: Start  Y: Files  X: Cancel  +: Install Now  B: Back\n");
            printf("Page %d/%d\n", current_page + 1, total_pages);

            u64 kDown = inputDown();
            u64 kNav = kDown | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
            if (kDown & HidNpadButton_B) {
                util::logLine("ui: back button pressed in downloads");
                return;
            }

            if (kNav & HidNpadButton_Down) {
                const int next_pos = (selected_pos + 1) % total_items;
                selected = static_cast<int>(order[next_pos]);
            }
            if (kNav & HidNpadButton_Up) {
                const int next_pos = (selected_pos - 1 + total_items) % total_items;
                selected = static_cast<int>(order[next_pos]);
            }
            if (kDown & HidNpadButton_A) {
                if (!downloads.startDownload(static_cast<size_t>(selected))) {
                    downloads.startNextDownload();
                }
            }
            if (kDown & HidNpadButton_Y) {
                showFilePicker(downloads, static_cast<size_t>(selected));
            }
            if (kDown & HidNpadButton_X) {
                downloads.cancelDownload(static_cast<size_t>(selected));
            }
            if (kDown & HidNpadButton_Plus) {
                downloads.startHybridInstall(static_cast<size_t>(selected));
            }
        } else {
            printf("Downloads\n");
            printf("Source: %s\n", downloads.dataSourceManager().modeDescription().c_str());
            printf("Items: 0 | Active: 0\n");
            printf("%s\n", layout::separatorLine('=').c_str());
            printf("Queue is empty.\n\n");
            printf("Add a torrent via catalog.\n\n");
            printf("B: Back\n");

            if (inputDown() & HidNpadButton_B) {
                util::logLine("ui: back button pressed in downloads");
                return;
            }
        }

        consoleUpdate(NULL);
    }
}

} // namespace ui
