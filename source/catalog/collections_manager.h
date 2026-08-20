#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Game;

namespace catalog {

struct CollectionEntry {
    std::string title;
    std::string title_id;
    double metacritic = 0.0;
    bool has_metacritic = false;
};

struct CollectionInfo {
    std::string id;          // ╨╕╨╝╤П ╤Д╨░╨╣╨╗╨░ ╨▒╨╡╨╖ ╤А╨░╤Б╤И╨╕╤А╨╡╨╜╨╕╤П .json
    std::string name;        // ╤А╤Г╤Б╤Б╨║╨╛╨╡ ╨╜╨░╨╖╨▓╨░╨╜╨╕╨╡ ╨┐╨╛╨┤╨▒╨╛╤А╨║╨╕
    std::string description; // ╤А╤Г╤Б╤Б╨║╨╛╨╡ ╨╛╨┐╨╕╤Б╨░╨╜╨╕╨╡
};

// ╨Я╤А╨╡╨┤╤Б╨╛╨▒╤А╨░╨╜╨╜╤Л╨╣ ╨╕╨╜╨┤╨╡╨║╤Б ╨┐╨╛ ╨║╨░╤В╨░╨╗╨╛╨│╤Г ╨┤╨╗╤П ╨▒╤Л╤Б╤В╤А╨╛╨│╨╛ ╨╝╨░╤В╤З╨╕╨╜╨│╨░ ╨▒╨╡╨╖ ╨┐╨╛╨╗╨╜╤Л╤Е ╨┐╤А╨╛╤Е╨╛╨┤╨╛╨▓.
struct CollectionMatchIndex {
    std::unordered_map<uint64_t, const Game*> by_title_id;
    std::vector<std::pair<std::string, const Game*>> norm_titles;
};

// ╨Я╨╛╤Б╤В╤А╨╛╨╕╤В╤М ╨╕╨╜╨┤╨╡╨║╤Б ╨╛╨┤╨╕╨╜ ╤А╨░╨╖ (╤В╤П╨╢╤С╨╗╨░╤П ╨╛╨┐╨╡╤А╨░╤Ж╨╕╤П, ╨▓╤Л╨╖╤Л╨▓╨░╤В╤М ╨╜╨░ ╨│╨╗╨░╨▓╨╜╨╛╨╝ ╨┐╨╛╤В╨╛╨║╨╡).
CollectionMatchIndex buildMatchIndex(const std::vector<Game>& games);

// ╨С╤Л╤Б╤В╤А╤Л╨╣ ╨╝╨░╤В╤З╨╕╨╜╨│ ╤З╨╡╤А╨╡╨╖ ╨╕╨╜╨┤╨╡╨║╤Б: title_id -> ╨╜╨╛╤А╨╝╨░╨╗╨╕╨╖╨╛╨▓╨░╨╜╨╜╨╛╨╡ ╨╜╨░╨╖╨▓╨░╨╜╨╕╨╡.
// ╨Т╨╛╨╖╨▓╤А╨░╤Й╨░╨╡╤В nullptr, ╨╡╤Б╨╗╨╕ ╨╜╨╡ ╨╜╨░╨╣╨┤╨╡╨╜╨╛.
const Game* matchWithIndex(const CollectionMatchIndex& index, const CollectionEntry& entry);

class CollectionsManager {
public:
    static CollectionsManager& instance();

    const std::vector<CollectionInfo>& collections() const { return collections_; }

    // ╨С╨╗╨╛╨║╨╕╤А╤Г╤О╤Й╨░╤П ╨╖╨░╨│╤А╤Г╨╖╨║╨░ ╨┐╨╛╨┤╨▒╨╛╤А╨║╨╕: ╤Б╨╜╨░╤З╨░╨╗╨░ ╨║╤Н╤И ╨╜╨░ SD, ╨╖╨░╤В╨╡╨╝ ╤Б╨╡╤В╤М,
    // ╨╡╤Б╨╗╨╕ ╨║╤Н╤И╨░ ╨╜╨╡╤В ╨╕╨╗╨╕ ╨╛╨╜ ╤Б╤В╨░╤А╤И╨╡ ╤Б╤Г╤В╨╛╨║. ╨Т╨╛╨╖╨▓╤А╨░╤Й╨░╨╡╤В false ╨┐╤А╨╕ ╨┐╨╛╨╗╨╜╨╛╨╣ ╨╜╨╡╤Г╨┤╨░╤З╨╡.
    bool loadCollection(const CollectionInfo& info, std::vector<CollectionEntry>& out_entries,
                        bool& from_cache);

    static std::string cachePathFor(const std::string& id);
    static bool isCacheFresh(const std::string& path, int max_age_seconds = 86400);

private:
    CollectionsManager();
    std::vector<CollectionInfo> collections_;
};

constexpr const char* kCollectionsBaseUrl =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/";

// ╨Я╨╛╨╕╤Б╨║ ╤А╨░╨╖╨┤╨░╤З╨╕ ╨▓ ╨║╨░╤В╨░╨╗╨╛╨│╨╡: ╤Б╨╜╨░╤З╨░╨╗╨░ ╨┐╨╛ title_id, ╨╖╨░╤В╨╡╨╝ ╨┐╨╛ ╨╜╨╛╤А╨╝╨░╨╗╨╕╨╖╨╛╨▓╨░╨╜╨╜╨╛╨╝╤Г ╨╜╨░╨╖╨▓╨░╨╜╨╕╤О.
// ╨Т╨╛╨╖╨▓╤А╨░╤Й╨░╨╡╤В nullptr, ╨╡╤Б╨╗╨╕ ╨╜╨╡ ╨╜╨░╨╣╨┤╨╡╨╜╨╛.
const Game* matchCollectionEntry(const std::vector<Game>& games, const CollectionEntry& entry);

} // namespace catalog
