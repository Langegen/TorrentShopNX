#include "ArchiveProgressDialog.hpp"
#include "../utils/file_ops.h"
#include "../utils/log.h"
#include <filesystem>
#include <algorithm>
#include <cmath>

namespace ui {

#if defined(__SWITCH__)
void ArchiveProgressDialog::threadEntry(void* arg) {
    auto* self = static_cast<ArchiveProgressDialog*>(arg);
    self->runExtraction();
}
#endif

ArchiveProgressDialog::ArchiveProgressDialog(
    const std::string& archivePath,
    const std::string& destDir,
    std::function<void(bool, const std::string&)> onComplete
) : ArchiveProgressDialog(new brls::Box(), archivePath, destDir, std::move(onComplete))
{
}

ArchiveProgressDialog::ArchiveProgressDialog(
    brls::Box* contentBox,
    const std::string& archivePath,
    const std::string& destDir,
    std::function<void(bool, const std::string&)> onComplete
) : brls::Dialog(contentBox),
    archivePath_(archivePath),
    destDir_(destDir),
    onComplete_(std::move(onComplete)),
    contentBox_(contentBox)
{
    util::logLine("ArchiveProgressDialog: constructor entered for " + archivePath_);
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
    progressBg_->setAxis(brls::Axis::ROW);
    progressBg_->setWidth(480.0f);
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
        if (currentFileLabel_) {
            currentFileLabel_->setText("Отмена распаковки...");
        }
    });

    this->setCancelable(true);

    auto* applet = this->getAppletFrame();
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }
    util::logLine("ArchiveProgressDialog: constructor completed");
}

ArchiveProgressDialog::~ArchiveProgressDialog() {
    util::logLine("ArchiveProgressDialog: destructor entered");
    if (aliveToken_) {
        aliveToken_->store(false);
    }
    if (cancelToken_) {
        cancelToken_->store(true);
    }

#if defined(__SWITCH__)
    if (threadStarted_) {
        threadWaitForExit(&thread_);
        threadClose(&thread_);
        threadStarted_ = false;
    }
#else
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
#endif
    util::logLine("ArchiveProgressDialog: destructor completed");
}

void ArchiveProgressDialog::updateUi(const util::ArchiveProgress& progress) {
    if (!aliveToken_ || !aliveToken_->load() || closed_.load()) return;

    if (currentFileLabel_) {
        std::string shortName = progress.currentFileName;
        if (shortName.size() > 45) {
            size_t start = shortName.size() - 42;
            while (start < shortName.size() && (static_cast<unsigned char>(shortName[start]) & 0xC0) == 0x80) {
                start++;
            }
            shortName = "..." + shortName.substr(start);
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
        if (pct > 0.0f && fillW < 4.0f) fillW = 4.0f;
        progressFill_->setWidth(fillW);
    }

    if (statsLabel_) {
        char buf[160];
        std::string extStr = util::formatFileSize(progress.bytesExtracted);
        uint64_t totalTarget = progress.totalUncompressedSize > 0
            ? progress.totalUncompressedSize
            : progress.totalArchiveSize;
        std::string totalStr = util::formatFileSize(totalTarget);
        float pct = progress.percentage;
        if (std::isnan(pct) || std::isinf(pct)) pct = 0.0f;
        pct = std::clamp(pct, 0.0f, 100.0f);

        if (progress.totalEntries > 0) {
            std::snprintf(buf, sizeof(buf), "%.1f%% · %s / %s (%zu / %zu)",
                          pct, extStr.c_str(), totalStr.c_str(),
                          progress.entriesProcessed, progress.totalEntries);
        } else {
            std::snprintf(buf, sizeof(buf), "%.1f%% · %s / %s (%zu)",
                          pct, extStr.c_str(), totalStr.c_str(),
                          progress.entriesProcessed);
        }
        statsLabel_->setText(buf);
    }
}

void ArchiveProgressDialog::startExtraction() {
    util::logLine("ArchiveProgressDialog: startExtraction opening dialog for " + archivePath_);
    this->open();
    util::logLine("ArchiveProgressDialog: dialog opened, creating extraction thread");

#if defined(__SWITCH__)
    // 512KB stack size (0x80000), default core (-2), Priority 0x2C
    Result rc = threadCreate(&thread_, &ArchiveProgressDialog::threadEntry, this, nullptr, 0x80000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        threadStarted_ = true;
        rc = threadStart(&thread_);
        if (R_FAILED(rc)) {
            util::logLine("ArchiveProgressDialog: threadStart failed, rc=" + std::to_string(rc));
            threadClose(&thread_);
            threadStarted_ = false;
            if (onComplete_) onComplete_(false, "Failed to start extraction thread");
            if (!closed_.exchange(true)) this->close();
        }
    } else {
        util::logLine("ArchiveProgressDialog: threadCreate failed, rc=" + std::to_string(rc));
        if (onComplete_) onComplete_(false, "Failed to create extraction thread");
        if (!closed_.exchange(true)) this->close();
    }
#else
    workerThread_ = std::thread([this]() { runExtraction(); });
#endif
}

void ArchiveProgressDialog::runExtraction() {
    auto alive = aliveToken_;
    auto cancel = cancelToken_;
    auto onComplete = onComplete_;
    std::string archivePath = archivePath_;
    std::string destDir = destDir_;

    auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    std::string err;
    bool ok = false;
    try {
        ok = util::extractArchive(
            archivePath,
            destDir,
            [this, alive, cancel, lastUpdate](const util::ArchiveProgress& prog) {
                if (!alive || !alive->load() || (cancel && cancel->load()) || closed_.load()) return;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count() > 80 || prog.percentage >= 99.9f) {
                    *lastUpdate = now;
                    brls::sync([this, alive, prog]() {
                        if (alive && alive->load() && !closed_.load()) {
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

    util::logLine("ArchiveProgressDialog: extraction finished, ok=" + std::to_string(ok) + " err=" + err);

    if (ok) {
        brls::sync([this, alive]() {
            if (alive && alive->load() && !closed_.load()) {
                if (progressFill_) {
                    float maxW = progressBg_ ? progressBg_->getWidth() : 480.0f;
                    if (maxW <= 0.0f) maxW = 480.0f;
                    progressFill_->setWidth(maxW);
                }
                if (statsLabel_) {
                    statsLabel_->setText("100.0% · Готово");
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    brls::sync([this, alive, ok, err, onComplete]() {
        if (!alive || !alive->load()) return;
        if (!closed_.exchange(true)) {
            this->close([onComplete, ok, err]() {
                if (onComplete) {
                    onComplete(ok, err);
                }
            });
        }
    });
}

} // namespace ui
