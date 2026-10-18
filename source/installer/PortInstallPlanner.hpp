#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include "torrent/torrent_manager.h"

namespace installer {

struct PortVariant {
    std::string id;            // "ru", "en", "modded", "vanilla", etc.
    std::string title;         // User-facing name (e.g. "Русская версия (RUS)")
    std::vector<int> file_indices;
    int64_t total_size = 0;
};

struct PortArchiveTask {
    int file_index = -1;
    std::string archive_torrent_path;
    std::string extract_dest_dir; // Usually "sdmc:/" or "sdmc:/switch/<game>/"
    int64_t compressed_size = 0;
};

struct PortInstallPlan {
    bool is_port = false;
    std::string port_game_name; // e.g. "codmw", "masseffect-nx", "nfsu2x", "Simpsons", etc.
    
    // NSP forwarder to install to Horizon OS
    int forwarder_nsp_index = -1;
    std::string forwarder_nsp_name;
    int64_t forwarder_nsp_size = 0;
    
    // Multiple variants (languages or mods), if detected
    std::vector<PortVariant> variants;
    int default_variant_index = 0;
    
    // Files that apply regardless of chosen variant (e.g. forwarder NSP, shared tools)
    std::vector<int> common_file_indices;
    
    // Mapping: torrent file index -> target path on SD card (e.g. "sdmc:/switch/codmw/game/...")
    std::unordered_map<int, std::string> file_destinations;
    
    // Archives to extract after downloading
    std::vector<PortArchiveTask> archives_to_extract;
    
    // Overlay files (e.g. from Homebrew/ or update folders) to copy/overwrite after archive extraction
    std::vector<int> overlay_file_indices;

    // Helper to get all file indices needed for a specific variant selection
    std::vector<int> getFilesForVariant(int variant_idx) const;
    
    // Helper to get only archives needed for a specific variant selection
    std::vector<PortArchiveTask> getArchivesForVariant(int variant_idx) const;
    
    // Helper to get total download size for a specific variant
    int64_t getDownloadSizeForVariant(int variant_idx, const std::vector<torrent::TorrentFileInfo>& all_files) const;
};

class PortInstallPlanner {
public:
    static PortInstallPlanner& instance();

    // Analyzes torrent file list and constructs an execution plan
    PortInstallPlan analyze(const std::string& release_title,
                            const std::vector<torrent::TorrentFileInfo>& files,
                            const std::string& preferred_lang = "ru");

private:
    std::string detectGameFolderName(const std::vector<torrent::TorrentFileInfo>& files,
                                     const std::string& release_title);
};

} // namespace installer
