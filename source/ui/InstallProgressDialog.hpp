#pragma once

#include <borealis.hpp>
#include "../datasource/file_data_source.h"
#include "../installer/hybrid_nsp_installer.h"
#include <atomic>
#include <memory>
#include <chrono>
#include <string>
#include <functional>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <thread>
#endif

namespace ui {

class InstallProgressDialog : public brls::Dialog {
public:
    InstallProgressDialog(
        const std::string& packagePath,
        int storageId, // 1 = SD card, 0 = NAND (Built-in user)
        std::function<void(bool success, const std::string& msg)> onComplete
    );
    ~InstallProgressDialog() override;

    void startInstallation();

private:
    InstallProgressDialog(
        brls::Box* contentBox,
        const std::string& packagePath,
        int storageId,
        std::function<void(bool success, const std::string& msg)> onComplete
    );

    void runInstallation();
    void updateUi(
        float progressPct,
        uint64_t bytesInstalled,
        uint64_t totalBytes,
        double speedKbps,
        const std::string& statusText
    );

#if defined(__SWITCH__)
    Thread thread_{};
    bool threadStarted_ = false;
    static void threadEntry(void* arg);
#else
    std::thread workerThread_;
#endif
    std::atomic<bool> closed_{false};

    std::string packagePath_;
    int storageId_;
    std::function<void(bool, const std::string&)> onComplete_;

    std::shared_ptr<std::atomic<bool>> cancelToken_;
    std::shared_ptr<std::atomic<bool>> aliveToken_;
    std::shared_ptr<installer::HybridNspInstaller> installer_;
    std::shared_ptr<datasource::FileDataSource> dataSource_;

    brls::Box* contentBox_ = nullptr;
    brls::Label* titleLabel_ = nullptr;
    brls::Label* targetStorageLabel_ = nullptr;
    brls::Label* statusLabel_ = nullptr;
    brls::Box* progressBg_ = nullptr;
    brls::Box* progressFill_ = nullptr;
    brls::Label* statsLabel_ = nullptr;

    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point lastUiUpdate_;
};

} // namespace ui
