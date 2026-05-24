#include "rss_parser.h"

#include <algorithm>

namespace rss {

std::string RssParser::extractTag(const std::string& block, const std::string& tag) {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    auto start = block.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    auto end = block.find(close, start);
    if (end == std::string::npos) return "";
    return block.substr(start, end - start);
}

std::vector<RssItem> RssParser::parse(const std::string& xml) {
    std::vector<RssItem> items;
    std::string::size_type pos = 0;
    while (true) {
        auto item_start = xml.find("<item", pos);
        if (item_start == std::string::npos) break;
        auto item_end = xml.find("</item>", item_start);
        if (item_end == std::string::npos) break;
        std::string block = xml.substr(item_start, item_end - item_start);

        RssItem item;
        item.title = extractTag(block, "title");
        item.magnet = extractTag(block, "magnet");
        if (item.magnet.empty()) {
            std::string link = extractTag(block, "link");
            if (link.rfind("magnet:", 0) == 0) {
                item.magnet = link;
            }
        }
        item.size = extractTag(block, "size");
        if (item.size.empty()) {
            // Try enclosure length="..."
            auto encl = block.find("enclosure");
            if (encl != std::string::npos) {
                auto len = block.find("length=\"", encl);
                if (len != std::string::npos) {
                    len += 8;
                    auto endq = block.find("\"", len);
                    if (endq != std::string::npos) {
                        item.size = block.substr(len, endq - len);
                    }
                }
            }
        }

        if (!item.title.empty() || !item.magnet.empty()) {
            items.push_back(item);
        }
        pos = item_end + 7;
    }
    return items;
}

} // namespace rss
