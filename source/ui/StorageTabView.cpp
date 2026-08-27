#include "StorageTabView.hpp"
#include "DownloadUiManager.hpp"
#include "../utils/storage_utils.h"
#include "../utils/switch_utils.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include <cstdio>

namespace ui {

namespace {

std::string formatBytes(unsigned long long bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    return std::string(buf);
}

constexpr float kBarWidth = 340.0f;
constexpr float kBarHeight = 16.0f;

} // namespace

StorageTabView::StorageTabView() : brls::Box(brls::Axis::COLUMN) {
    this->setWidth(10000);
    this->setHeight(brls::View::AUTO);
    this->setPaddingTop(30);
    this->setPaddingRight(40);
    this->setPaddingBottom(40);
    this->setPaddingLeft(40);
    this->setAlignItems(brls::AlignItems::STRETCH);

    // ---- Хранилище ----
    addSectionHeader(this, "app/settings/storage_section"_i18n);
    addStorageRow(this, "app/settings/storage_sd"_i18n, &sdTrack_, &sdFill_, &sdInfo_);
    addStorageRow(this, "app/settings/storage_nand"_i18n, &nandTrack_, &nandFill_, &nandInfo_);

    // ---- Кэш ----
    addSectionHeader(this, "app/settings/cache_section"_i18n);
    addCacheRow("app/settings/cache_thumbnails"_i18n, TSNX_CACHE_THUMBNAILS, false);
    addCacheRow("app/settings/cache_catalog"_i18n, TSNX_CACHE_CATALOG, false);
    addCacheRow("app/settings/cache_meta"_i18n, TSNX_CACHE_META, false);
    addCacheRow("app/settings/cache_collections"_i18n, TSNX_CACHE_COLLECTIONS, false);
    addCacheRow("app/settings/cache_icons"_i18n, TSNX_CACHE_ICONS, false);
    addCacheRow("app/settings/cache_versions"_i18n, TSNX_VERSIONS_PATH, true);
    addCacheRow("app/settings/cache_temp"_i18n, "", false,
                 []() {
                     return util::dirSizeRecursive(TSNX_CACHE_STREAM) +
                            util::dirSizeRecursive(TSNX_CACHE_TMP) +
                            util::dirSizeRecursive(TSNX_CACHE_LOCALENGINE);
                 },
                 []() {
                     return util::deleteDirContents(TSNX_CACHE_STREAM) +
                            util::deleteDirContents(TSNX_CACHE_TMP) +
                            util::deleteDirContents(TSNX_CACHE_LOCALENGINE);
                 });

    for (size_t i = 0; i < rows_.size(); ++i) {
        addCacheCell(this, i);
    }

    // ---- Очистить весь кэш ----
    {
        auto* cell = new brls::DetailCell();
        cell->setText("app/settings/cache_clear_all"_i18n);
        cell->setDetailText("app/settings/storage_calculating"_i18n);
        allSizeLabel_ = cell->detail;
        cell->registerClickAction([this](brls::View* view) {
            if (busy_) return true;
            uint64_t total = 0;
            for (uint64_t sz : lastSizes_) total += sz;
            brls::Dialog* dlg = new brls::Dialog(
                brls::getStr("app/settings/storage_confirm_clear_all", formatBytes(total)));
            dlg->addButton("app/common/yes"_i18n, [this]() {
                runClearAll();
            });
            dlg->addButton("app/common/no"_i18n, []() {});
            dlg->open();
            return true;
        });
        this->addView(cell);
    }

    // ---- Прерванные установки ----
    addSectionHeader(this, "app/settings/placeholder_section"_i18n);
    addPlaceholderCell(this);

    this->registerAction("app/settings/storage_refresh"_i18n, brls::ControllerButton::BUTTON_X,
                         [this](brls::View* view) {
                             refreshAll();
                             return true;
                         });

    refreshAll();
}

StorageTabView::~StorageTabView() {
    *alive_ = false;
}

void StorageTabView::addSectionHeader(brls::Box* parent, const std::string& title) {
    auto* header = new brls::Label();
    header->setFontSize(18);
    header->setText(title);
    header->setTextColor(nvgRGBA(255, 255, 255, 150));
    header->setMarginTop(25);
    header->setMarginBottom(10);
    parent->addView(header);
}

void StorageTabView::addStorageRow(brls::Box* parent, const std::string& title,
                                   brls::Box** out_track, brls::Rectangle** out_fill,
                                   brls::Label** out_info) {
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(10000);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setMarginBottom(14);

    auto* titleLabel = new brls::Label();
    titleLabel->setFontSize(18);
    titleLabel->setText(title);
    titleLabel->setWidth(190);
    row->addView(titleLabel);

    auto* track = new brls::Box(brls::Axis::ROW);
    track->setWidth(kBarWidth);
    track->setHeight(kBarHeight);
    track->setCornerRadius(kBarHeight / 2.0f);
    track->setBorderThickness(1.0f);
    track->setBorderColor(nvgRGBA(255, 255, 255, 70));
    track->setBackgroundColor(nvgRGBA(255, 255, 255, 28));
    row->addView(track);

    auto* fill = new brls::Rectangle();
    fill->setWidth(0);
    fill->setHeight(kBarHeight - 4.0f);
    fill->setCornerRadius((kBarHeight - 4.0f) / 2.0f);
    fill->setColor(nvgRGB(76, 175, 80));
    fill->setMarginTop(2);
    fill->setMarginBottom(2);
    fill->setMarginLeft(2);
    track->addView(fill);

    auto* info = new brls::Label();
    info->setFontSize(15);
    info->setText("app/settings/storage_unknown"_i18n);
    info->setWidth(240);
    info->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    row->addView(info);

    parent->addView(row);
    *out_track = track;
    *out_fill = fill;
    *out_info = info;
}

void StorageTabView::addCacheRow(const std::string& title, const std::string& path, bool is_file,
                                 const std::function<uint64_t()>& computeFn,
                                 const std::function<uint64_t()>& clearFn) {
    CacheRow row;
    row.title = title;
    row.path = path;
    row.is_file = is_file;
    if (clearFn) {
        row.clear = clearFn;
    } else if (is_file) {
        row.clear = [path]() { return util::deleteFile(path); };
    } else {
        row.clear = [path]() { return util::deleteDirContents(path); };
    }
    if (computeFn) {
        row.compute = computeFn;
    }
    rows_.push_back(std::move(row));
}

void StorageTabView::addCacheCell(brls::Box* parent, size_t index) {
    auto* cell = new brls::DetailCell();
    cell->setText(rows_[index].title);
    cell->setDetailText("app/settings/storage_calculating"_i18n);
    rows_[index].sizeLabel = cell->detail;

    cell->registerClickAction([this, index](brls::View* view) {
        if (busy_) return true;
        uint64_t size = (index < lastSizes_.size()) ? lastSizes_[index] : 0;
        brls::Dialog* dlg = new brls::Dialog(
            brls::getStr("app/settings/storage_confirm_clear", rows_[index].title, formatBytes(size)));
        dlg->addButton("app/common/yes"_i18n, [this, index]() {
            runClear(index);
        });
        dlg->addButton("app/common/no"_i18n, []() {});
        dlg->open();
        return true;
    });

    parent->addView(cell);
}

void StorageTabView::addPlaceholderCell(brls::Box* parent) {
    auto* cell = new brls::DetailCell();
    cell->setText("app/settings/storage_placeholders"_i18n);
    cell->setDetailText("app/settings/storage_calculating"_i18n);
    placeholderDetail_ = cell->detail;

    cell->registerClickAction([this](brls::View* view) {
        if (busy_) return true;
        if (ui::DownloadManager::instance().getImpl().hasActiveTransfers()) {
            brls::Dialog* dlg = new brls::Dialog("app/settings/storage_placeholder_busy"_i18n);
            dlg->addButton("app/common/ok"_i18n, []() {});
            dlg->open();
            return true;
        }
        if (placeholderCount_ == 0) {
            brls::Dialog* dlg = new brls::Dialog("app/settings/storage_placeholder_none"_i18n);
            dlg->addButton("app/common/ok"_i18n, []() {});
            dlg->open();
            return true;
        }
        brls::Dialog* dlg = new brls::Dialog(brls::getStr(
            "app/settings/storage_confirm_placeholders",
            std::to_string(placeholderCount_), formatBytes(placeholderSize_)));
        dlg->addButton("app/common/yes"_i18n, [this]() {
            runCleanupPlaceholders();
        });
        dlg->addButton("app/common/no"_i18n, []() {});
        dlg->open();
        return true;
    });

    parent->addView(cell);
}

void StorageTabView::updateBar(brls::Box* track, brls::Rectangle* fill, brls::Label* info,
                               int64_t free_space, int64_t total_space) {
    (void)track;
    double used = (total_space > free_space) ? (double)(total_space - free_space) : 0.0;
    double pct = (total_space > 0) ? used / (double)total_space : 0.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;

    float w = (float)(kBarWidth * pct) - 4.0f;
    if (w < 0.0f) w = 0.0f;
    fill->setWidth(w);

    NVGcolor c = nvgRGB(76, 175, 80);
    if (pct >= 0.90) c = nvgRGB(244, 67, 54);
    else if (pct >= 0.75) c = nvgRGB(255, 193, 7);
    fill->setColor(c);

    info->setText(brls::getStr("app/settings/storage_free_of",
                               formatBytes(static_cast<unsigned long long>(free_space)),
                               formatBytes(static_cast<unsigned long long>(total_space))));
}

void StorageTabView::refreshAll() {
    if (busy_) return;
    busy_ = true;
    auto alive = alive_;

    struct Desc {
        std::string path;
        bool is_file;
        std::function<uint64_t()> compute;
    };
    std::vector<Desc> descs;
    for (const auto& r : rows_) {
        Desc d;
        d.path = r.path;
        d.is_file = r.is_file;
        d.compute = r.compute;
        descs.push_back(std::move(d));
    }

    brls::async([alive, descs, this]() {
        std::vector<uint64_t> sizes(descs.size(), 0);
        for (size_t i = 0; i < descs.size(); ++i) {
            if (descs[i].compute) {
                sizes[i] = descs[i].compute();
            } else if (!descs[i].path.empty()) {
                if (descs[i].is_file) {
                    sizes[i] = util::pathSize(descs[i].path);
                } else {
                    sizes[i] = util::dirSizeRecursive(descs[i].path);
                }
            }
        }

        int phCountSd = 0, phCountNand = 0;
        int64_t phSizeSd = 0, phSizeNand = 0;
        util::getLeftoverPlaceholders(1, phCountSd, phSizeSd);
        util::getLeftoverPlaceholders(0, phCountNand, phSizeNand);

        int64_t sdFree = 0, sdTotal = 0, nandFree = 0, nandTotal = 0;
        bool sdOk = util::getStorageStats(1, sdFree, sdTotal);
        bool nandOk = util::getStorageStats(0, nandFree, nandTotal);

        brls::sync([alive, sizes, phCountSd, phSizeSd, phCountNand, phSizeNand,
                    sdOk, sdFree, sdTotal, nandOk, nandFree, nandTotal, this]() {
            if (!*alive) return; // view уже уничтожен (вкладка переключена)

            busy_ = false;
            lastSizes_ = sizes;

            for (size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].sizeLabel) {
                    uint64_t sz = (i < sizes.size()) ? sizes[i] : 0;
                    rows_[i].sizeLabel->setText(sz > 0 ? formatBytes(sz) : "app/settings/storage_empty"_i18n);
                }
            }

            int phCount = phCountSd + phCountNand;
            placeholderCount_ = phCount;
            placeholderSize_ = static_cast<uint64_t>(phSizeSd) + static_cast<uint64_t>(phSizeNand);
            if (placeholderDetail_) {
                if (phCount > 0) {
                    placeholderDetail_->setText(brls::getStr(
                        "app/settings/storage_placeholders_detail",
                        std::to_string(phCount), formatBytes(placeholderSize_)));
                } else {
                    placeholderDetail_->setText("app/settings/storage_placeholder_none_short"_i18n);
                }
            }

            uint64_t total = 0;
            for (uint64_t sz : lastSizes_) total += sz;
            if (allSizeLabel_) {
                allSizeLabel_->setText(total > 0 ? formatBytes(total) : "app/settings/storage_empty"_i18n);
            }

            if (sdOk) updateBar(sdTrack_, sdFill_, sdInfo_, sdFree, sdTotal);
            else sdInfo_->setText("app/settings/storage_unknown"_i18n);
            if (nandOk) updateBar(nandTrack_, nandFill_, nandInfo_, nandFree, nandTotal);
            else nandInfo_->setText("app/settings/storage_unknown"_i18n);
        });
    });
}

void StorageTabView::runClear(size_t index) {
    if (busy_ || index >= rows_.size()) return;
    busy_ = true;
    auto alive = alive_;
    std::string title = rows_[index].title;
    auto clearFn = rows_[index].clear; // копия: вызов на фоновом потоке

    brls::async([alive, title, clearFn, this]() {
        uint64_t freed = clearFn ? clearFn() : 0;
        brls::sync([alive, title, freed, this]() {
            if (!*alive) return;
            busy_ = false;
            if (freed > 0) {
                brls::Application::notify(brls::getStr("app/settings/storage_cleared", title, formatBytes(freed)));
            } else {
                brls::Application::notify("app/settings/storage_cleared_none"_i18n);
            }
            refreshAll();
        });
    });
}

void StorageTabView::runClearAll() {
    if (busy_) return;
    busy_ = true;
    auto alive = alive_;

    std::vector<std::function<uint64_t()>> fns;
    for (auto& r : rows_) {
        fns.push_back(r.clear);
    }

    brls::async([alive, fns, this]() {
        uint64_t freed = 0;
        for (auto& fn : fns) {
            if (fn) freed += fn();
        }
        brls::sync([alive, freed, this]() {
            if (!*alive) return;
            busy_ = false;
            if (freed > 0) {
                brls::Application::notify(brls::getStr("app/settings/storage_cleared_all", formatBytes(freed)));
            } else {
                brls::Application::notify("app/settings/storage_cleared_none"_i18n);
            }
            refreshAll();
        });
    });
}

void StorageTabView::runCleanupPlaceholders() {
    if (busy_) return;
    busy_ = true;
    auto alive = alive_;

    brls::async([alive, this]() {
        int countSd = 0, countNand = 0;
        int64_t freedSd = 0, freedNand = 0;
        bool okSd = util::cleanupLeftoverPlaceholders(1, countSd, freedSd);
        bool okNand = util::cleanupLeftoverPlaceholders(0, countNand, freedNand);
        uint64_t freed = static_cast<uint64_t>(freedSd) + static_cast<uint64_t>(freedNand);

        brls::sync([alive, freed, okSd, okNand, this]() {
            if (!*alive) return;
            busy_ = false;
            if (!okSd && !okNand) {
                brls::Application::notify("app/settings/storage_placeholder_error"_i18n);
            } else if (freed > 0) {
                brls::Application::notify(brls::getStr("app/settings/storage_placeholders_cleared", formatBytes(freed)));
            } else {
                brls::Application::notify("app/settings/storage_cleared_none"_i18n);
            }
            refreshAll();
        });
    });
}

} // namespace ui