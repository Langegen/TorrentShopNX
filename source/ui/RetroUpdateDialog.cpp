#include "RetroUpdateDialog.hpp"
#include "../catalog/retro_catalog_manager.h"
#include "../utils/log.h"
#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <string>

namespace ui {

void showRetroCatalogUpdateDialog(std::function<void(int updatedCount)> onComplete) {
    auto& mgr = catalog::RetroCatalogManager::instance();
    int cachedCount = mgr.getTotalCachedGamesCount();

    std::string msg = "Обновление баз данных ретро-игр с GitHub.\n";
    if (cachedCount > 0) {
        msg += "В кэше сохранено игр: " + std::to_string(cachedCount) + ".\n";
    }
    msg += "Выберите режим:";

    auto* chooseDialog = new brls::Dialog(msg);
    chooseDialog->setCancelable(true);

    auto runUpdate = [onComplete](bool only_cached) {
        auto* content = new brls::Box();
        content->setAxis(brls::Axis::COLUMN);
        content->setWidth(480.0f);
        content->setPadding(20.0f);
        content->setAlignItems(brls::AlignItems::STRETCH);

        auto* titleLabel = new brls::Label();
        titleLabel->setText("Обновление баз ретро-игр");
        titleLabel->setFontSize(20.0f);
        titleLabel->setTextColor(nvgRGB(255, 255, 255));
        titleLabel->setMarginBottom(10.0f);
        content->addView(titleLabel);

        auto* statusLabel = new brls::Label();
        statusLabel->setText("Подготовка...");
        statusLabel->setFontSize(14.0f);
        statusLabel->setTextColor(nvgRGB(180, 180, 190));
        statusLabel->setMarginBottom(14.0f);
        content->addView(statusLabel);

        auto* progressBg = new brls::Box();
        progressBg->setAxis(brls::Axis::ROW);
        progressBg->setWidth(440.0f);
        progressBg->setHeight(10.0f);
        progressBg->setCornerRadius(5.0f);
        progressBg->setBackgroundColor(nvgRGBA(42, 45, 52, 255));
        progressBg->setMarginBottom(8.0f);

        auto* progressFill = new brls::Box();
        progressFill->setWidth(0.0f);
        progressFill->setHeight(10.0f);
        progressFill->setCornerRadius(5.0f);
        progressFill->setBackgroundColor(nvgRGB(0, 224, 165));
        progressBg->addView(progressFill);
        content->addView(progressBg);

        auto* counterLabel = new brls::Label();
        counterLabel->setText("0%");
        counterLabel->setFontSize(13.0f);
        counterLabel->setTextColor(nvgRGB(150, 150, 160));
        counterLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        content->addView(counterLabel);

        auto* progressDialog = new brls::Dialog(content);
        progressDialog->setCancelable(false);

        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        auto closedFlag = std::make_shared<std::atomic<bool>>(false);

        std::string cancelText = brls::getStr("app/common/cancel");
        if (cancelText.empty() || cancelText == "app/common/cancel") cancelText = "Отмена";

        progressDialog->addButton(cancelText, [cancelFlag, statusLabel]() {
            cancelFlag->store(true);
            statusLabel->setText("Отмена обновления...");
        });

        progressDialog->open();

        brls::async([cancelFlag, closedFlag, progressDialog, statusLabel, progressFill, counterLabel, only_cached, onComplete]() {
            auto& mgr = catalog::RetroCatalogManager::instance();

            std::vector<catalog::RetroConsoleInfo> targets;
            for (const auto& c : mgr.consoles()) {
                if (!only_cached || mgr.isConsoleCatalogCached(c.id)) {
                    targets.push_back(c);
                }
            }

            if (targets.empty()) {
                brls::sync([progressDialog, closedFlag, onComplete]() {
                    if (!closedFlag->exchange(true)) {
                        progressDialog->close([onComplete]() {
                            brls::Application::notify("Нет сохраненных баз для обновления");
                            if (onComplete) onComplete(0);
                        });
                    }
                });
                return;
            }

            int total = static_cast<int>(targets.size());
            int successCount = 0;

            for (int i = 0; i < total; ++i) {
                if (cancelFlag->load()) break;

                const auto& c = targets[i];
                int current = i + 1;

                brls::sync([statusLabel, progressFill, counterLabel, current, total, c]() {
                    statusLabel->setText(c.name + " (" + std::to_string(current) + "/" + std::to_string(total) + ")");
                    float pct = static_cast<float>(current) / static_cast<float>(total);
                    progressFill->setWidth(440.0f * pct);
                    counterLabel->setText(std::to_string(static_cast<int>(pct * 100)) + "%");
                });

                std::vector<Game> dummy;
                if (mgr.refreshConsoleCatalog(c.id, dummy, nullptr)) {
                    successCount++;
                }
            }

            bool cancelled = cancelFlag->load();

            brls::sync([progressDialog, closedFlag, cancelled, successCount, total, onComplete]() {
                if (!closedFlag->exchange(true)) {
                    progressDialog->close([cancelled, successCount, total, onComplete]() {
                        if (cancelled) {
                            brls::Application::notify("Обновление прервано (" + std::to_string(successCount) + "/" + std::to_string(total) + ")");
                        } else if (successCount > 0) {
                            brls::Application::notify("Обновлено платформ: " + std::to_string(successCount) + " из " + std::to_string(total));
                        } else {
                            brls::Application::notify("Ошибка обновления (проверьте интернет)");
                        }
                        if (onComplete) onComplete(successCount);
                    });
                }
            });
        });
    };

    chooseDialog->addButton("Кэшированные", [runUpdate]() {
        runUpdate(true);
    });

    chooseDialog->addButton("Все 20 платформ", [runUpdate]() {
        runUpdate(false);
    });

    chooseDialog->addButton("app/common/cancel"_i18n, []() {});

    chooseDialog->open();
}

} // namespace ui
