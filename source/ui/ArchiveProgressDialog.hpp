#pragma once

#include <borealis.hpp>
#include "../utils/archive_utils.h"
#include <atomic>
#include <memory>
#include <chrono>

namespace ui {

class ArchiveProgressDialog : public brls::Dialog {
public:
    ArchiveProgressDialog(const std::string& archivePath, const std::string& destDir, std::function<void(bool success, const std::string& msg)> onComplete);
    ~ArchiveProgressDialog() override;

    void startExtraction();

private:
    void updateUi(const util::ArchiveProgress& progress);

    std::string archivePath_;
    std::string destDir_;
    std::function<void(bool, const std::string&)> onComplete_;

    std::shared_ptr<std::atomic<bool>> cancelToken_;
    std::shared_ptr<std::atomic<bool>> aliveToken_;

    brls::Box* contentBox_ = nullptr;
    brls::Label* titleLabel_ = nullptr;
    brls::Label* currentFileLabel_ = nullptr;
    brls::Box* progressBg_ = nullptr;
    brls::Box* progressFill_ = nullptr;
    brls::Label* statsLabel_ = nullptr;

    std::chrono::steady_clock::time_point lastUiUpdate_;
};

} // namespace ui
