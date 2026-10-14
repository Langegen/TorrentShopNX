#include "PortInstallPlanner.hpp"
#include "utils/log.h"
#include "utils/app_paths.h"
#include "utils/archive_utils.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace installer {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool endsWith(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

// Normalize path separators to forward slashes
std::string normalizePath(const std::string& path) {
    std::string res = path;
    for (char& c : res) {
        if (c == '\\') c = '/';
    }
    // Remove leading slashes
    while (!res.empty() && res.front() == '/') res.erase(res.begin());
    return res;
}

// Extract path starting from "switch/"
bool extractSwitchSubpath(const std::string& norm_path, std::string& out_after_switch) {
    std::string lower = toLower(norm_path);
    size_t idx = lower.find("switch/");
    if (idx != std::string::npos) {
        out_after_switch = norm_path.substr(idx + 7); // skip "switch/"
        return true;
    }
    if (lower == "switch") {
        out_after_switch = "";
        return true;
    }
    return false;
}

} // namespace

PortInstallPlanner& PortInstallPlanner::instance() {
    static PortInstallPlanner inst;
    return inst;
}

std::vector<int> PortInstallPlan::getFilesForVariant(int variant_idx) const {
    std::vector<int> res = common_file_indices;
    if (variant_idx >= 0 && variant_idx < static_cast<int>(variants.size())) {
        for (int idx : variants[variant_idx].file_indices) {
            res.push_back(idx);
        }
    } else if (variants.empty()) {
        // If no variants, everything mapped is included
        for (const auto& pair : file_destinations) {
            if (std::find(res.begin(), res.end(), pair.first) == res.end()) {
                res.push_back(pair.first);
            }
        }
    }
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    return res;
}

std::vector<PortArchiveTask> PortInstallPlan::getArchivesForVariant(int variant_idx) const {
    if (variants.empty()) {
        return archives_to_extract;
    }
    auto needed = getFilesForVariant(variant_idx);
    std::unordered_set<int> neededSet(needed.begin(), needed.end());
    std::vector<PortArchiveTask> result;
    for (const auto& a : archives_to_extract) {
        if (neededSet.count(a.file_index)) {
            result.push_back(a);
        }
    }
    return result;
}

int64_t PortInstallPlan::getDownloadSizeForVariant(int variant_idx,
                                                   const std::vector<torrent::TorrentFileInfo>& all_files) const {
    auto needed = getFilesForVariant(variant_idx);
    std::unordered_map<int, int64_t> sizeMap;
    for (const auto& f : all_files) {
        sizeMap[f.index] = f.size;
    }
    int64_t total = 0;
    for (int idx : needed) {
        auto it = sizeMap.find(idx);
        if (it != sizeMap.end()) {
            total += it->second;
        }
    }
    return total;
}

std::string PortInstallPlanner::detectGameFolderName(const std::vector<torrent::TorrentFileInfo>& files,
                                                     const std::string& release_title) {
    // 1. Inspect paths under switch/<folder>/
    for (const auto& f : files) {
        std::string p = normalizePath(f.name);
        std::string afterSwitch;
        if (extractSwitchSubpath(p, afterSwitch) && !afterSwitch.empty()) {
            size_t slash = afterSwitch.find('/');
            std::string folder = (slash != std::string::npos) ? afterSwitch.substr(0, slash) : afterSwitch;
            if (!folder.empty() && !endsWith(folder, ".nro") && !endsWith(folder, ".txt")) {
                return folder;
            }
        }
    }

    // 2. Fallback to title sanitization
    std::string clean = release_title;
    // Strip brackets
    size_t br = clean.find('[');
    if (br != std::string::npos) clean = clean.substr(0, br);
    size_t paren = clean.find('(');
    if (paren != std::string::npos) clean = clean.substr(0, paren);
    
    std::string safe;
    for (char c : clean) {
        if (std::isalnum(static_cast<unsigned char>(c))) safe += c;
    }
    return safe.empty() ? "game" : toLower(safe);
}

PortInstallPlan PortInstallPlanner::analyze(const std::string& release_title,
                                           const std::vector<torrent::TorrentFileInfo>& files,
                                           const std::string& preferred_lang) {
    PortInstallPlan plan;
    if (files.empty()) return plan;

    bool has_nro = false;
    bool has_switch_folder = false;
    bool has_nsp = false;
    bool has_archive = false;

    for (const auto& f : files) {
        std::string norm = normalizePath(f.name);
        std::string lower = toLower(norm);
        if (endsWith(lower, ".nro")) has_nro = true;
        if (lower.find("switch/") != std::string::npos) has_switch_folder = true;
        if (endsWith(lower, ".nsp")) has_nsp = true;
        if (endsWith(lower, ".7z") || endsWith(lower, ".zip") || endsWith(lower, ".rar")) has_archive = true;
    }

    bool title_is_port = containsIgnoreCase(release_title, "порт") ||
                         containsIgnoreCase(release_title, "хоумбрю") ||
                         containsIgnoreCase(release_title, "homebrew") ||
                         containsIgnoreCase(release_title, "recomp") ||
                         containsIgnoreCase(release_title, "openmw") ||
                         containsIgnoreCase(release_title, "xash3d");

    // Only qualify as a port if it has port indicators
    if (!has_nro && !has_switch_folder && !(has_nsp && title_is_port) && !has_archive) {
        return plan;
    }

    plan.is_port = true;
    plan.port_game_name = detectGameFolderName(files, release_title);

    // 1. Detect Forwarder NSP
    int best_nsp_idx = -1;
    int64_t best_nsp_size = 0;
    std::string best_nsp_name;

    for (const auto& f : files) {
        std::string norm = normalizePath(f.name);
        if (endsWith(norm, ".nsp")) {
            // Forwarder NSPs are typically small (< 50MB, usually ~300KB-5MB)
            // If multiple NSPs exist, prefer the one with "(port)", "[v0]", or Title ID in brackets
            bool has_title_id = (norm.find('[') != std::string::npos && norm.find(']') != std::string::npos);
            bool has_port_tag = containsIgnoreCase(norm, "port") || containsIgnoreCase(norm, "homebrew");

            if (best_nsp_idx == -1 || has_port_tag || has_title_id) {
                best_nsp_idx = f.index;
                best_nsp_size = f.size;
                best_nsp_name = std::filesystem::path(norm).filename().string();
                if (has_port_tag) break; // Found exact port forwarder
            }
        }
    }

    plan.forwarder_nsp_index = best_nsp_idx;
    plan.forwarder_nsp_name = best_nsp_name;
    plan.forwarder_nsp_size = best_nsp_size;

    if (best_nsp_idx >= 0) {
        plan.common_file_indices.push_back(best_nsp_idx);
        std::string fn = std::filesystem::path(best_nsp_name).filename().string();
        plan.file_destinations[best_nsp_idx] = std::string(TSNX_CACHE_TMP) + "/" + fn;
    }

    // 2. Detect Variants (Archives or Folders)
    // Check for archive pairs (Jak and Daxter, NFS U2, Morrowind)
    int ru_archive_idx = -1;
    int en_archive_idx = -1;
    int modded_archive_idx = -1;
    int vanilla_archive_idx = -1;

    for (const auto& f : files) {
        std::string norm = normalizePath(f.name);
        std::string lower = toLower(norm);
        if (endsWith(lower, ".7z") || endsWith(lower, ".zip") || endsWith(lower, ".rar")) {
            if (lower.find("russ") != std::string::npos || lower.find("rus") != std::string::npos) {
                ru_archive_idx = f.index;
            } else if (lower.find("mod") != std::string::npos || lower.find("better pack") != std::string::npos) {
                modded_archive_idx = f.index;
            } else if (lower.find("vanilla") != std::string::npos) {
                vanilla_archive_idx = f.index;
            } else if (lower == "switch.7z" || lower == "switch.zip" || lower.find("eng") != std::string::npos) {
                en_archive_idx = f.index;
            }
        }
    }

    // Check for folder pairs or optional translation mods (CoD 4, Mass Effect 1, Torchlight 1, OpenMoHAA, Halo CE)
    bool has_ru_folder = false;
    bool has_en_folder = false;
    for (const auto& f : files) {
        std::string lower = toLower(normalizePath(f.name));
        if (lower.rfind("russian", 0) == 0 || lower.find("/russian") != std::string::npos ||
            lower.find("russian language") != std::string::npos ||
            lower.find("russian voiceover") != std::string::npos ||
            lower.find("rus mod") != std::string::npos) {
            has_ru_folder = true;
        }
        if (lower.rfind("english", 0) == 0 || lower.find("/english") != std::string::npos ||
            lower.find("english version") != std::string::npos ||
            lower.find("eng mod") != std::string::npos) {
            has_en_folder = true;
        }
    }

    if (ru_archive_idx >= 0 && en_archive_idx >= 0) {
        // Archetype 3: Dual Archives RUS vs ENG (Jak and Daxter, NFS Underground 2)
        PortVariant vRu;
        vRu.id = "ru";
        vRu.title = "Русская версия (RUS)";
        vRu.file_indices.push_back(ru_archive_idx);

        PortVariant vEn;
        vEn.id = "en";
        vEn.title = "Английская версия (ENG)";
        vEn.file_indices.push_back(en_archive_idx);

        for (const auto& f : files) {
            if (f.index != ru_archive_idx && f.index != en_archive_idx && f.index != plan.forwarder_nsp_index) {
                plan.common_file_indices.push_back(f.index);
            }
        }

        plan.variants.push_back(vRu);
        plan.variants.push_back(vEn);
    } else if (modded_archive_idx >= 0 && vanilla_archive_idx >= 0) {
        // Dual Archives Modded vs Vanilla (Morrowind)
        PortVariant vMod;
        vMod.id = "modded";
        vMod.title = "Сборка с модами (Better Pack)";
        vMod.file_indices.push_back(modded_archive_idx);

        PortVariant vVan;
        vVan.id = "vanilla";
        vVan.title = "Оригинальная версия (Vanilla)";
        vVan.file_indices.push_back(vanilla_archive_idx);

        for (const auto& f : files) {
            if (f.index != modded_archive_idx && f.index != vanilla_archive_idx && f.index != plan.forwarder_nsp_index) {
                plan.common_file_indices.push_back(f.index);
            }
        }

        plan.variants.push_back(vMod);
        plan.variants.push_back(vVan);
    } else if (has_ru_folder && has_en_folder) {
        // Archetype 4: Dual Folders RUS vs ENG (CoD 4, Mass Effect 1, Torchlight 1)
        PortVariant vRu;
        vRu.id = "ru";
        vRu.title = "Русская версия (RUS)";

        PortVariant vEn;
        vEn.id = "en";
        vEn.title = "Английская версия (ENG)";

        for (const auto& f : files) {
            if (f.index == plan.forwarder_nsp_index) continue;
            std::string lower = toLower(normalizePath(f.name));
            if (lower.find("russian") != std::string::npos) {
                vRu.file_indices.push_back(f.index);
            } else if (lower.find("english") != std::string::npos) {
                vEn.file_indices.push_back(f.index);
            } else {
                // If common/shared (e.g. root switch/ in Torchlight or Homebrew updates)
                plan.common_file_indices.push_back(f.index);
            }
        }

        plan.variants.push_back(vRu);
        plan.variants.push_back(vEn);
    } else if (has_ru_folder && !has_en_folder) {
        // Base game in switch/ with optional Russian voiceover / translation mod (e.g. Halo CE, OpenMoHAA)
        PortVariant vRu;
        vRu.id = "ru";
        vRu.title = "Русская версия (с озвучкой / переводом)";

        PortVariant vEn;
        vEn.id = "en";
        vEn.title = "Оригинальная английская версия (ENG)";

        std::unordered_set<std::string> overridden_subpaths;
        for (const auto& f : files) {
            std::string norm = normalizePath(f.name);
            std::string lower = toLower(norm);
            if (lower.find("russian") != std::string::npos) {
                std::string afterSwitch;
                if (extractSwitchSubpath(norm, afterSwitch)) {
                    overridden_subpaths.insert(toLower(afterSwitch));
                }
            }
        }

        for (const auto& f : files) {
            if (f.index == plan.forwarder_nsp_index) continue;
            std::string norm = normalizePath(f.name);
            std::string lower = toLower(norm);
            if (lower.find("russian") != std::string::npos) {
                vRu.file_indices.push_back(f.index);
            } else {
                std::string afterSwitch;
                bool is_overridden = false;
                if (extractSwitchSubpath(norm, afterSwitch)) {
                    if (overridden_subpaths.count(toLower(afterSwitch)) > 0) {
                        is_overridden = true;
                    }
                }
                if (is_overridden) {
                    vEn.file_indices.push_back(f.index);
                } else {
                    plan.common_file_indices.push_back(f.index);
                }
            }
        }

        plan.variants.push_back(vRu);
        plan.variants.push_back(vEn);
    } else {
        // Single default variant (Archetypes 1 and 2: Subway Surfers, LEGO Batman 3, Celeste 64, etc.)
        for (const auto& f : files) {
            if (f.index != plan.forwarder_nsp_index) {
                plan.common_file_indices.push_back(f.index);
            }
        }
        PortVariant vDef;
        vDef.id = "default";
        vDef.title = "Стандартная установка";
        plan.variants.push_back(vDef);
    }

    // Set default variant according to preferred language
    plan.default_variant_index = 0;
    if (plan.variants.size() > 1 && (preferred_lang == "en" || preferred_lang == "en-US")) {
        for (size_t i = 0; i < plan.variants.size(); ++i) {
            if (plan.variants[i].id == "en" || plan.variants[i].id == "vanilla") {
                plan.default_variant_index = static_cast<int>(i);
                break;
            }
        }
    }

    // 3. Path Remapping & Task Creation
    for (const auto& f : files) {
        if (f.index == plan.forwarder_nsp_index) continue;

        std::string norm = normalizePath(f.name);
        std::string lower = toLower(norm);

        // Check if file is an archive
        if (endsWith(lower, ".7z") || endsWith(lower, ".zip") || endsWith(lower, ".rar")) {
            PortArchiveTask at;
            at.file_index = f.index;
            at.archive_torrent_path = norm;
            at.compressed_size = f.size;
            at.extract_dest_dir = "sdmc:/"; // Default extract root for switch.7z/sdcard.zip

            std::string fn = std::filesystem::path(norm).filename().string();
            plan.file_destinations[f.index] = std::string(TSNX_CACHE_TMP) + "/" + fn;
            plan.archives_to_extract.push_back(at);
            continue;
        }

        // Check if file is an overlay/homebrew update (should overwrite after extract)
        if (lower.find("homebrew") != std::string::npos || lower.find("update") != std::string::npos) {
            plan.overlay_file_indices.push_back(f.index);
        }

        // Map destination on SD card
        std::string afterSwitch;
        if (extractSwitchSubpath(norm, afterSwitch)) {
            // Found switch/... -> route to sdmc:/switch/<afterSwitch>
            plan.file_destinations[f.index] = "sdmc:/switch/" + afterSwitch;
        } else if (norm.rfind("openmw/", 0) == 0 || norm.rfind("retroarch/", 0) == 0) {
            // Root-level game directory
            plan.file_destinations[f.index] = "sdmc:/" + norm;
        } else {
            // Fallback: place into sdmc:/switch/<game_folder>/<filename>
            std::string fn = std::filesystem::path(norm).filename().string();
            plan.file_destinations[f.index] = "sdmc:/switch/" + plan.port_game_name + "/" + fn;
        }
    }

    // Calculate variant sizes
    for (auto& v : plan.variants) {
        v.total_size = plan.getDownloadSizeForVariant(&v - &plan.variants[0], files);
    }

    util::logLine("PortInstallPlanner: analyzed '" + release_title + "' -> is_port=" +
                  (plan.is_port ? "true" : "false") + " game='" + plan.port_game_name +
                  "' forwarder=" + (plan.forwarder_nsp_index >= 0 ? plan.forwarder_nsp_name : "none") +
                  " variants=" + std::to_string(plan.variants.size()) +
                  " archives=" + std::to_string(plan.archives_to_extract.size()));

    return plan;
}

} // namespace installer
