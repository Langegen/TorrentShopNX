#include "InstallProgressDialog.hpp"
#include "../utils/file_ops.h"
#include "../utils/switch_utils.h"
#include "../utils/log.h"
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <thread>

namespace ui {

namespace {

uint64_t extractTitleId(const std::string& name) {
    size_t start = name.find('[');
    while (start != std::string::npos) {
        size_t end = name.find(']', start);
        if (end != std::string::npos && (end - start) == 17) {
            std::string tid_str = name.substr(start + 1, 16);
            try {
                return std::stoull(tid_str, nullptr, 16);
            } catch (...) {}
        }
        start = name.find('[', start + 1);
    }
    return 0;
}

std::string getRussianPhaseText(installer::InstallState state) {
    switch (state) {
        case installer::InstallState::Idle:
            return "Инициализация...";
        case installer::InstallState::ParsingHeader:
            return "Чтение заголовка пакета...";
        case installer::InstallState::CreatingPlaceHolders:
            return "Подготовка хранилища (NCM)...";
        case installer::InstallState::Streaming:
            return "Установка контента...";
        case installer::InstallState::InstallingTickets:
            return "Установка тикетов (ES)...";
        case installer::InstallState::RegisteringMeta:
            return "Регистрация в системе...";
        case installer::InstallState::Completed:
            return "Установка успешно завершена!";
        case installer::InstallState::Failed:
            return "Ошибка при установке";
        case installer::InstallState::Cancelled:
            return "Отмена установки...";
        default:
            return "Обработка...";
    }
}

} // namespace

InstallProgressDialog::InstallProgressDialog(
    const std::string& packagePath,
    int storageId,
    std::function<void(bool, const std::string&)> onComplete
) : brls::Dialog(contentBox_ = new brls::Box()),
    packagePath_(packagePath),
    storageId_(storageId),
    onComplete_(std::move(onComplete))
{
    cancelToken_ = std::make_shared<std::atomic<bool>>(false);
    aliveToken_ = std::make_shared<std::atomic<bool>>(true);
    startTime_ = std::chrono::steady_clock::now();
    lastUiUpdate_ = startTime_;

    // Modal Box styling (Emerald / Turquoise theme with dark slate background)
    contentBox_->setAxis(brls::Axis::COLUMN);
    contentBox_->setWidth(520.0f);
    contentBox_->setPadding(20.0f, 22.0f, 18.0f, 22.0f);
    contentBox_->setAlignItems(brls::AlignItems::STRETCH);

    std::filesystem::path ap(packagePath_);
    std::string fileName = ap.filename().generic_string();

    // ── 1. Header with Emerald Accent ──────────────────────────────────────
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(0, 224, 165, 80)); // Emerald line

    // Gamepad Badge
    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 35)); // Emerald tint

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE0E0"); // Gamepad icon
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(nvgRGB(0, 224, 165)); // Emerald icon
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    titleLabel_ = new brls::Label();
    titleLabel_->setText(fileName);
    titleLabel_->setFontSize(18.0f);
    titleLabel_->setTextColor(nvgRGB(255, 255, 255));
    titleLabel_->setSingleLine(true);
    headerTextCol->addView(titleLabel_);

    targetStorageLabel_ = new brls::Label();
    std::string storageText = (storageId_ == 1)
        ? "Целевое хранилище: SD-карта"
        : "Целевое хранилище: Память консоли (NAND)";
    targetStorageLabel_->setText(storageText);
    targetStorageLabel_->setFontSize(13.0f);
    targetStorageLabel_->setTextColor(nvgRGBA(0, 224, 165, 220)); // Emerald subtitle
    targetStorageLabel_->setSingleLine(true);
    headerTextCol->addView(targetStorageLabel_);

    headerBox->addView(headerTextCol);
    contentBox_->addView(headerBox);

    // ── 2. Status Label ────────────────────────────────────────────────────
    statusLabel_ = new brls::Label();
    statusLabel_->setText("Инициализация установки...");
    statusLabel_->setFontSize(14.0f);
    statusLabel_->setTextColor(nvgRGB(190, 195, 205));
    statusLabel_->setMarginBottom(12.0f);
    contentBox_->addView(statusLabel_);

    // ── 3. Progress Bar (Emerald fill on dark rail) ────────────────────────
    progressBg_ = new brls::Box();
    progressBg_->setWidthPercentage(100.0f);
    progressBg_->setHeight(10.0f);
    progressBg_->setCornerRadius(5.0f);
    progressBg_->setBackgroundColor(nvgRGBA(42, 45, 52, 255));
    progressBg_->setMarginBottom(10.0f);

    progressFill_ = new brls::Box();
    progressFill_->setWidth(0.0f);
    progressFill_->setHeight(10.0f);
    progressFill_->setCornerRadius(5.0f);
    progressFill_->setBackgroundColor(nvgRGB(0, 224, 165)); // Emerald fill
    progressBg_->addView(progressFill_);
    contentBox_->addView(progressBg_);

    // ── 4. Stats Label ─────────────────────────────────────────────────────
    statsLabel_ = new brls::Label();
    statsLabel_->setText("0.0% · 0 B");
    statsLabel_->setFontSize(13.0f);
    statsLabel_->setTextColor(nvgRGB(150, 155, 165));
    statsLabel_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    contentBox_->addView(statsLabel_);

    // Cancel Button
    std::string cancelText = brls::getStr("app/common/cancel");
    if (cancelText.empty() || cancelText == "app/common/cancel") cancelText = "Отмена";

    this->addButton(cancelText, [this]() {
        if (cancelToken_) {
            cancelToken_->store(true);
        }
        if (installer_) {
            installer_->cancel();
        }
        if (statusLabel_) {
            statusLabel_->setText("Отмена установки...");
        }
    });

    this->setCancelable(true);

    auto* applet = dynamic_cast<brls::AppletFrame*>(this->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }
}

InstallProgressDialog::~InstallProgressDialog() {
    if (aliveToken_) {
        aliveToken_->store(false);
    }
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    if (installer_) {
        installer_->cancel();
    }
    if (dataSource_) {
        dataSource_->close();
    }
}

void InstallProgressDialog::updateUi(
    float progressPct,
    uint64_t bytesInstalled,
    uint64_t totalBytes,
    double speedKbps,
    const std::string& statusText
) {
    if (!aliveToken_ || !aliveToken_->load()) return;

    if (statusLabel_) {
        statusLabel_->setText(statusText);
    }

    if (progressFill_ && progressBg_) {
        float maxW = progressBg_->getWidth();
        if (maxW <= 0.0f) maxW = 476.0f;
        float pct = progressPct;
        if (std::isnan(pct) || std::isinf(pct)) pct = 0.0f;
        pct = std::clamp(pct, 0.0f, 100.0f);
        float fillW = (maxW * pct) / 100.0f;
        progressFill_->setWidth(fillW);
    }

    if (statsLabel_) {
        char buf[160];
        std::string instStr = util::formatFileSize(bytesInstalled);
        std::string totalStr = util::formatFileSize(totalBytes);
        float pct = progressPct;
        if (std::isnan(pct) || std::isinf(pct)) pct = 0.0f;
        pct = std::clamp(pct, 0.0f, 100.0f);

        std::string speedStr;
        if (speedKbps > 1024.0) {
            char sBuf[32];
            std::snprintf(sBuf, sizeof(sBuf), "%.1f МБ/с", speedKbps / 1024.0);
            speedStr = sBuf;
        } else if (speedKbps > 0.0) {
            char sBuf[32];
            std::snprintf(sBuf, sizeof(sBuf), "%.0f КБ/с", speedKbps);
            speedStr = sBuf;
        }

        std::string etaStr;
        if (speedKbps > 100.0 && totalBytes > bytesInstalled) {
            double remainBytes = static_cast<double>(totalBytes - bytesInstalled);
            double secRemain = remainBytes / (speedKbps * 1024.0);
            int s = static_cast<int>(secRemain);
            if (s < 60) {
                etaStr = " · Ост. " + std::to_string(s) + "с";
            } else {
                int m = s / 60;
                int remS = s % 60;
                etaStr = " · Ост. " + std::to_string(m) + "м " + std::to_string(remS) + "с";
            }
        }

        if (!speedStr.empty()) {
            std::snprintf(buf, sizeof(buf), "%.1f%% · %s / %s · %s%s",
                          pct, instStr.c_str(), totalStr.c_str(), speedStr.c_str(), etaStr.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%.1f%% · %s / %s%s",
                          pct, instStr.c_str(), totalStr.c_str(), etaStr.c_str());
        }
        statsLabel_->setText(buf);
    }
}

void InstallProgressDialog::startInstallation() {
    this->open();

    auto alive = aliveToken_;
    auto cancel = cancelToken_;
    auto onComplete = onComplete_;
    std::string packagePath = packagePath_;
    int storageId = storageId_;

    util::logLine("InstallProgressDialog: startInstallation for " + packagePath + " storage=" + std::to_string(storageId));

    brls::async([this, alive, cancel, onComplete, packagePath, storageId]() {
        dataSource_ = std::make_shared<datasource::FileDataSource>(packagePath);
        dataSource_->setCancelFlag(cancel.get());

        if (!dataSource_->open()) {
            util::logLine("InstallProgressDialog: failed to open package data source: " + packagePath);
            brls::sync([this, onComplete]() {
                this->close([onComplete]() {
                    if (onComplete) onComplete(false, "Не удалось открыть файл пакета");
                });
            });
            return;
        }

        uint64_t fileTotalSize = dataSource_->totalSize();

        installer_ = std::make_shared<installer::HybridNspInstaller>();

        std::filesystem::path pp(packagePath);
        std::string fileName = pp.filename().generic_string();
        installer_->setSourceFileNameHint(fileName);
        installer_->setHintTitleId(extractTitleId(fileName));

        installer::InstallConfig config;
        config.buffer_size = 64 * 1024 * 1024;
        config.chunk_size = 4 * 1024 * 1024;
        config.verify_sha256 = true;
        config.install_ticket = true;
#ifdef __SWITCH__
        config.storage = (storageId == 1) ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
#else
        config.storage = storageId;
#endif

        if (!installer_->start(dataSource_.get(), config)) {
            std::string errMsg = installer_->errorMessage();
            if (errMsg.empty()) errMsg = "Не удалось запустить установщик пакета";
            util::logLine("InstallProgressDialog: installer start failed: " + errMsg);

            if (dataSource_) dataSource_->close();

            brls::sync([this, onComplete, errMsg]() {
                this->close([onComplete, errMsg]() {
                    if (onComplete) onComplete(false, errMsg);
                });
            });
            return;
        }

        // Monitoring and UI update loop
        auto lastUiTime = std::chrono::steady_clock::now();
        while (alive->load() && !installer_->isFinished()) {
            if (cancel->load()) {
                installer_->cancel();
                break;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUiTime).count();
            if (elapsedMs >= 150) {
                lastUiTime = now;

                float prog = installer_->progress() * 100.0f;
                uint64_t instBytes = installer_->bytesInstalled();
                uint64_t totalBytes = installer_->totalBytes();
                if (totalBytes == 0) totalBytes = fileTotalSize;
                double speed = installer_->downloadSpeedKbps();
                std::string statusText = getRussianPhaseText(installer_->state());

                brls::sync([this, prog, instBytes, totalBytes, speed, statusText]() {
                    this->updateUi(prog, instBytes, totalBytes, speed, statusText);
                });
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }

        // If cancelled, wait for graceful shutdown
        if (cancel->load()) {
            installer_->cancel();
            int waitIters = 0;
            while (!installer_->isFinished() && waitIters < 40) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waitIters++;
            }
        }

        bool success = (installer_->state() == installer::InstallState::Completed);
        std::string finalMsg;
        if (cancel->load()) {
            finalMsg = "Установка отменена";
        } else if (success) {
            finalMsg = "Установка успешно завершена!";
        } else {
            finalMsg = installer_->errorMessage();
            if (finalMsg.empty()) finalMsg = "Произошла ошибка при установке пакета";
        }

        util::logLine("InstallProgressDialog: finished, success=" + std::string(success ? "yes" : "no") + " msg=" + finalMsg);

        if (dataSource_) {
            dataSource_->close();
        }

        brls::sync([this, onComplete, success, finalMsg]() {
            this->close([onComplete, success, finalMsg]() {
                if (onComplete) onComplete(success, finalMsg);
            });
        });
    });
}

} // namespace ui
