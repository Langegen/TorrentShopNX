#pragma once

#include <string>
#include <vector>

namespace rss {

struct RssItem {
    std::string title;
    std::string magnet;
    std::string size;
};

class RssParser {
public:
    std::vector<RssItem> parse(const std::string& xml);

private:
    std::string extractTag(const std::string& block, const std::string& tag);
};

} // namespace rss
