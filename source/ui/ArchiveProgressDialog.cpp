#include "ArchiveProgressDialog.hpp"
#include "../utils/file_ops.h"
#include "../utils/log.h"
#include <filesystem>

namespace ui {

ArchiveProgressDialog::ArchiveProgressDialog(
    const std::string& archivePath,
    const std::string& destDir,
    std::function<void(bool, const std::string&)> onComplete
) : brls::Dialog(contentBox_ = new brls::Box()),
    archivePath_(archivePath),
    destDir_(destDir),
    onComplete_(std::move(onComplete))
{
    cancelToken_ = std::make_shared<std::atomic<bool>>(false);
    aliveToken_ = std::make_shared<std::atomic<bool>>(true);
    lastUiUpdate_ = std::chrono::steady_clock::now();

    contentBox_->setAxis(brls::Axis::COLUMN);
    contentBox_->setWidth(520.0f);
    contentBox_->setPadding(20.0f);
    contentBox_->setAlignItems(brls::AlignItems::STRETCH);

    std::filesystem::path ap(archivePath_);
    std::string fileName = ap.filename().generic_string();

    titleLabel_ = new brls::Label();
    titleLabel_->setText(fileName);
    titleLabel_->setFontSize(20);
    titleLabel_->setTextColor(nvgRGB(255, 255, 255));
    titleLabel_->setMarginBottom(10.0f);
    contentBox_->addView(titleLabel_);

    currentFileLabel_ = new brls::Label();
    currentFileLabel_->setText("...");
    currentFileLabel_->setFontSize(14);
    currentFileLabel_->setTextColor(nvgRGB(180, 180, 190));
    currentFileLabel_->setMarginBottom(14.0f);
    contentBox_->addView(currentFileLabel_);

    progressBg_ = new brls::Box();
    progressBg_->setWidthPercentage(100.0f);
    progressBg_->setHeight(10.0f);
    progressBg_->setCornerRadius(5.0f);
    progressBg_->setBackgroundColor(nvgRGBA(42, 45, 52, 255));
    progressBg_->setMarginBottom(8.0f);

    progressFill_ = new brls::Box();
    progressFill_->setWidth(0.0f);
    progressFill_->setHeight(10.0f);
    progressFill_->setCornerRadius(5.0f);
    progressFill_->setBackgroundColor(nvgRGB(0, 224, 165));
    progressBg_->addView(progressFill_);
    contentBox_->addView(progressBg_);

    statsLabel_ = new brls::Label();
    statsLabel_->setText("0.0% · 0 B");
    statsLabel_->setFontSize(13);
    statsLabel_->setTextColor(nvgRGB(150, 150, 160));
    statsLabel_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    contentBox_->addView(statsLabel_);

    std::string cancelText = brls::getStr("app/common/cancel");
    if (cancelText.empty() || cancelText == "app/common/cancel") cancelText = "Отмена";

    this->addButton(cancelText, [this]() {
        if (cancelToken_) {
            cancelToken_->store(true);
        }
        this->close();
    });

    this->setCancelable(true);

    auto* applet = dynamic_cast<brls::AppletFrame*>(this->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }
}

ArchiveProgressDialog::~ArchiveProgressDialog() {
    if (aliveToken_) {
        aliveToken_->store(false);
    }
    if (cancelToken_) {
        cancelToken_->store(true);
    }
}

void ArchiveProgressDialog::updateUi(const util::ArchiveProgress& progress) {
    if (!aliveToken_ || !aliveToken_->load()) return;

    if (currentFileLabel_) {
        std::string shortName = progress.currentFileName;
        if (shortName.size() > 45) {
            shortName = "..." + shortName.substr(shortName.size() - 42);
        }
        currentFileLabel_->setText(shortName);
    }

    if (progressFill_ && progressBg_) {
        float maxW = progressBg_->getWidth();
        if (maxW <= 0.0f) maxW = 480.0f;
        float pct = progress.percentage;
        if (std::isnan(pct) || std::isinf(pct)) pct = 0.0f;
        pct = std::clamp(pct, 0.0f, 100.0f);
        float fillW = (maxW * pct) / 100.0f;
        progressFill_->setWidth(fillW);
    }

    if (statsLabel_) {
        char buf[128];
        std::string extStr = util::formatFileSize(progress.bytesExtracted);
        std::string totalStr = util::formatFileSize(progress.totalArchiveSize);
        float pct = progress.percentage;
        if (std::isnan(pct) || std::isinf(pct)) pct = 0.0f;
        pct = std::clamp(pct, 0.0f, 100.0f);
        std::snprintf(buf, sizeof(buf), "%.1f%% · %s / %s (%zu)",
                      pct, extStr.c_str(), totalStr.c_str(), progress.entriesProcessed);
        statsLabel_->setText(buf);
    }
}

void ArchiveProgressDialog::startExtraction() {
    this->open();

    auto alive = aliveToken_;
    auto cancel = cancelToken_;
    auto onComplete = onComplete_;
    std::string archivePath = archivePath_;
    std::string destDir = destDir_;

    util::logLine("ArchiveProgressDialog: startExtraction async task started");

    brls::async([this, alive, cancel, onComplete, archivePath, destDir]() {
        auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        std::string err;
        bool ok = false;
        try {
            ok = util::extractArchive(
                archivePath,
                destDir,
                [this, alive, cancel, lastUpdate](const util::ArchiveProgress& prog) {
                    if (!alive || !alive->load() || (cancel && cancel->load())) return;
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count() > 80 || prog.percentage >= 99.9f) {
                        *lastUpdate = now;
                        brls::sync([this, alive, prog]() {
                            if (alive && alive->load()) {
                                updateUi(prog);
                            }
                        });
                    }
                },
                cancel,
                err
            );
        } catch (const std::exception& e) {
            err = std::string("Исключение при распаковке: ") + e.what();
            util::logLine("ArchiveProgressDialog: exception in extractArchive: " + err);
            ok = false;
        } catch (...) {
            err = "Неизвестная ошибка при распаковке";
            util::logLine("ArchiveProgressDialog: unknown exception in extractArchive");
            ok = false;
        }

        brls::sync([this, alive, ok, err, onComplete]() {
            if (!alive || !alive->load()) return;
            this->close([onComplete, ok, err]() {
                if (onComplete) {
                    onComplete(ok, err);
                }
            });
        });
    });
}

} // namespace ui
