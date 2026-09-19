#pragma once

#include <borealis.hpp>
#include "../utils/archive_utils.h"
#include <atomic>
#include <memory>
#include <chrono>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <thread>
#endif

namespace ui {

enum class ArchiveOpMode { Extract, Create };

class ArchiveProgressDialog : public brls::Dialog {
public:
    // Extraction
    ArchiveProgressDialog(const std::string& archivePath, const std::string& destDir, std::function<void(bool success, const std::string& msg)> onComplete);

    // Creation
    ArchiveProgressDialog(const std::string& targetArchivePath, const std::vector<std::string>& sourcePaths, const std::string& baseDir, std::function<void(bool success, const std::string& msg)> onComplete);

    ~ArchiveProgressDialog() override;

    void startExtraction();
    void startCreation();

private:
    ArchiveProgressDialog(brls::Box* contentBox, const std::string& archivePath, const std::string& destDir, std::function<void(bool success, const std::string& msg)> onComplete);
    ArchiveProgressDialog(brls::Box* contentBox, const std::string& targetArchivePath, const std::vector<std::string>& sourcePaths, const std::string& baseDir, std::function<void(bool success, const std::string& msg)> onComplete);

    void initDialogUi(const std::string& titleText, const std::string& subText);
    void runExtraction();
    void runCreation();
    void updateUi(const util::ArchiveProgress& progress);

#if defined(__SWITCH__)
    Thread thread_{};
    bool threadStarted_ = false;
    static void threadEntry(void* arg);
#else
    std::thread workerThread_;
#endif

    ArchiveOpMode mode_ = ArchiveOpMode::Extract;
    std::string archivePath_;
    std::string destDir_;
    std::vector<std::string> sourcePaths_;
    std::string baseDir_;
    std::function<void(bool, const std::string&)> onComplete_;

    std::shared_ptr<std::atomic<bool>> cancelToken_;
    std::shared_ptr<std::atomic<bool>> aliveToken_;
    std::atomic<bool> closed_{false};

    brls::Box* contentBox_ = nullptr;
    brls::Label* titleLabel_ = nullptr;
    brls::Label* currentFileLabel_ = nullptr;
    brls::Box* progressBg_ = nullptr;
    brls::Box* progressFill_ = nullptr;
    brls::Label* statsLabel_ = nullptr;

    std::chrono::steady_clock::time_point lastUiUpdate_;
};

} // namespace ui
