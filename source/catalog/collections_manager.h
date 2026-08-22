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
    std::string id;          // имя файла без расширения .json
    std::string name;        // русское название по умолчанию
    std::string description; // русское описание по умолчанию

    std::string getName() const;
    std::string getDescription() const;
};

// Предсобранный индекс по каталогу для быстрого матчинга без полных проходов.
struct CollectionMatchIndex {
    std::unordered_map<uint64_t, const Game*> by_title_id;
    std::vector<std::pair<std::string, const Game*>> norm_titles;
};

// Построить индекс один раз (тяжёлая операция, вызывать на главном потоке).
CollectionMatchIndex buildMatchIndex(const std::vector<Game>& games);

// Быстрый матчинг через индекс: title_id -> нормализованное название.
// Возвращает nullptr, если не найдено.
const Game* matchWithIndex(const CollectionMatchIndex& index, const CollectionEntry& entry);

class CollectionsManager {
public:
    static CollectionsManager& instance();

    const std::vector<CollectionInfo>& collections() const { return collections_; }

    // Блокирующая загрузка подборки: сначала кэш на SD, затем сеть,
    // если кэша нет или он старше суток. Возвращает false при полной неудаче.
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

// Поиск раздачи в каталоге: сначала по title_id, затем по нормализованному названию.
// Возвращает nullptr, если не найдено.
const Game* matchCollectionEntry(const std::vector<Game>& games, const CollectionEntry& entry);

} // namespace catalog
