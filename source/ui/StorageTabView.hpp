#pragma once

#include <borealis.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// Вкладка «Хранилище и кэш»: место на SD/NAND с прогрессбарами, очистка кэша
// по категориям (с объёмом освобождения) и очистка прерванных установок.
class StorageTabView : public brls::Box {
public:
    StorageTabView();
    ~StorageTabView() override;

private:
    struct CacheRow {
        std::string title;                  // i18n-ключ названия
        std::string path;                   // путь (пусто = произвольная очистка)
        bool is_file = false;               // одиночный файл vs каталог
        std::function<uint64_t()> compute;  // размер (по умолчанию pathSize)
        std::function<uint64_t()> clear;    // очистка (по умолчанию по path)
        brls::Label* sizeLabel = nullptr;
    };

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<CacheRow> rows_;
    std::vector<uint64_t> lastSizes_;
    bool busy_ = false;

    brls::Box* sdTrack_ = nullptr;
    brls::Rectangle* sdFill_ = nullptr;
    brls::Label* sdInfo_ = nullptr;
    brls::Box* nandTrack_ = nullptr;
    brls::Rectangle* nandFill_ = nullptr;
    brls::Label* nandInfo_ = nullptr;

    int placeholderCount_ = 0;
    uint64_t placeholderSize_ = 0;
    brls::Label* placeholderDetail_ = nullptr;
    brls::Label* allSizeLabel_ = nullptr;

    void addSectionHeader(brls::Box* parent, const std::string& title);
    void addStorageRow(brls::Box* parent, const std::string& title, brls::Box** out_track,
                       brls::Rectangle** out_fill, brls::Label** out_info);
    void addCacheRow(const std::string& title, const std::string& path, bool is_file,
                     const std::function<uint64_t()>& computeFn = std::function<uint64_t()>(),
                     const std::function<uint64_t()>& clearFn = std::function<uint64_t()>());
    void addCacheCell(brls::Box* parent, size_t index);
    void addPlaceholderCell(brls::Box* parent);

    void refreshAll();
    void updateBar(brls::Box* track, brls::Rectangle* fill, brls::Label* info,
                   int64_t free_space, int64_t total_space);
    void runClear(size_t index);
    void runClearAll();
    void runCleanupPlaceholders();
};

} // namespace ui