#include "browser.h"
#include "details.h"
#include "console_layout.h"

#include <switch.h>
#include <algorithm>

#include "input.h"

namespace ui {

void showCatalogBrowser(const std::vector<catalog::CatalogEntry>& entries, download::DownloadManager& downloads) {
    layout::useFullScreenWindow();

    if (entries.empty()) {
        consoleClear();
        printf("No catalog entries loaded.\n\nPress B to return.\n");
        while (appletMainLoop()) {
            downloads.trackProgress();
            if (inputDown() & HidNpadButton_B) return;
            consoleUpdate(NULL);
        }
        return;
    }

    int selected = 0;

    while (appletMainLoop()) {
        downloads.trackProgress();
        layout::useFullScreenWindow();
        consoleClear();

        const int width = layout::printableWidth();
        const int height = layout::consoleHeight();
        const int header_lines = 4;
        const int footer_lines = 3;
        const int item_height = 4;
        const int page_size = std::max(1, (height - header_lines - footer_lines) / item_height);
        const int total_items = static_cast<int>(entries.size());
        selected = layout::clampIndex(selected, total_items);

        const int current_page = selected / page_size;
        const int start = current_page * page_size;
        const int end = std::min(start + page_size, total_items);
        const int total_pages = layout::pageCount(total_items, page_size);

        printf("Catalog Browser\n");
        printf("Items: %d | Selected: %d/%d\n", total_items, selected + 1, total_items);
        printf("A: Details  B: Back\n");
        printf("%s\n", layout::separatorLine('=').c_str());

        for (int i = start; i < end; ++i) {
            const auto& e = entries[i];
            std::string meta;
            if (!e.size.empty()) meta += e.size;
            if (!e.category.empty()) {
                if (!meta.empty()) meta += " | ";
                meta += e.category;
            }

            const auto title_lines = layout::fixedTextBlock(
                e.title,
                static_cast<size_t>(std::max(8, width - 4)),
                2);
            const std::string meta_line = layout::ellipsizeUtf8(
                meta.empty() ? " " : meta,
                static_cast<size_t>(std::max(8, width - 4)));
            const bool has_second_title = !title_lines[1].empty();

            printf("%s %s\n",
                   i == selected ? ">" : " ",
                   layout::encodeForConsole(title_lines[0]).c_str());
            if (has_second_title) {
                printf("  %s\n", layout::encodeForConsole(title_lines[1]).c_str());
                printf("  %s\n", layout::encodeForConsole(meta_line).c_str());
            } else {
                printf("  %s\n", layout::encodeForConsole(meta_line).c_str());
                printf("  \n");
            }
            printf("\n");
        }

        printf("%s\n", layout::separatorLine('=').c_str());
        printf("Page %d/%d\n", current_page + 1, total_pages);
        printf("Use Up/Down to browse\n");

        u64 kDown = inputDown();
        u64 kNav = kDown | inputRepeat(HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right);
        if (kNav & HidNpadButton_Down) selected = (selected + 1) % total_items;
        if (kNav & HidNpadButton_Up) selected = (selected - 1 + total_items) % total_items;
        if (kNav & HidNpadButton_Right) selected = std::min(selected + page_size, total_items - 1);
        if (kNav & HidNpadButton_Left) selected = std::max(selected - page_size, 0);
        if (kDown & HidNpadButton_A) {
            showEntryDetails(entries[selected], downloads);
        }
        if (kDown & HidNpadButton_B) return;
        consoleUpdate(NULL);
    }
}

} // namespace ui
