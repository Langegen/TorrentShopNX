#pragma once

// Downloads and installs one emulator package (an .nro, an archive, a set of
// files, or a Tico module) without any UI, so it can run from the emulators
// screen and from a game download alike.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "retro_emulator_manager.h"

namespace catalog {

struct InstallProgress {
    enum class Stage {
        Downloading,  // file_index / file_count are set for multi-file packages
        Extracting,
        Finishing
    };
    Stage stage = Stage::Downloading;
    std::string file;        // file being downloaded or unpacked
    int file_index = 0;      // 0-based
    int file_count = 1;
    int64_t done = 0;        // bytes (downloading) or percent * 100 (extracting)
    int64_t total = 0;
    float fraction = 0.0f;   // 0..1 for the whole package
};

using InstallProgressFn = std::function<void(const InstallProgress&)>;

// Installs pkg and records its version. Runs synchronously: call it off the UI
// thread. Returns false with out_error set on failure or cancel.
bool installEmulatorPackage(const EmulatorPackage& pkg,
                            const InstallProgressFn& progress,
                            const std::shared_ptr<std::atomic<bool>>& cancel,
                            std::string& out_error);

} // namespace catalog
