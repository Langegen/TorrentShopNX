#include "FileSelectView.hpp"
#include "style/UiTokens.hpp"
#include "style/HeaderHints.hpp"
#include "dashboard/DashboardIcon.hpp"
#include "../utils/file_ops.h"
#include "../utils/i18n_plural.h"
#include "DownloadUiManager.hpp"
#include "MainMenu.hpp"
#include "DownloadsView.hpp"
#include "ThemeManager.hpp"
#include "../datasource/custom_engine_client.h"
#include "../config/config.h"
#include "../catalog/retro_catalog_manager.h"
#include "../utils/switch_utils.h"
#include "../net/image_downloader.h"
#include "../installer/nsp_header.h"
#include "../installer/cnmt_parser.h"
#include "../installer/ncz_parser.h"
#include "../utils/app_paths.h"
#include "../catalog/release_finder.h"
#include "../catalog/game_version.hpp"
#include "GameDetailView.hpp"
#include "ReleaseChooser.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <thread>

extern std::recursive_mutex g_switch_service_mutex;

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// Custom NanoVG Widgets (VectorCheckboxView, ToggleSwitchView, StorageBarView)
// ─────────────────────────────────────────────────────────────────────────────

static NVGcolor contrastTextOn(NVGcolor bg) {
    const float lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    return lum > 0.62f ? nvgRGB(24, 26, 32) : nvgRGB(255, 255, 255);
}

static NVGcolor chipInactiveBg() {
    return style::isLight() ? nvgRGBA(226, 231, 238, 220) : nvgRGBA(255, 255, 255, 24);
}

static NVGcolor rowCardBg() {
    return style::isLight() ? nvgRGBA(255, 255, 255, 220) : nvgRGBA(255, 255, 255, 16);
}

static NVGcolor rowCardBorder() {
    return style::isLight() ? nvgRGBA(205, 214, 226, 180) : nvgRGBA(255, 255, 255, 28);
}

static NVGcolor pillNeutralBg() {
    return style::isLight() ? nvgRGBA(226, 231, 238, 255) : nvgRGBA(255, 255, 255, 28);
}

class VectorCheckboxView : public brls::View {
public:
    enum class State { Unchecked, Checked, Partial, Disabled };

    explicit VectorCheckboxView(float size = 24.0f) : size_(size) {
        setWidth(size);
        setHeight(size);
        setFocusable(false);
    }

    void setState(State s) { state_ = s; }
    State getState() const { return state_; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        const float boxSize = std::min(width, height);
        const float bx = x + (width - boxSize) * 0.5f;
        const float by = y + (height - boxSize) * 0.5f;
        const float r = 6.0f;

        if (state_ == State::Checked) {
            NVGcolor fill = ThemeManager::instance().getAccentColor();
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, by, boxSize, boxSize, r);
            nvgFillColor(vg, fill);
            nvgFill(vg);

            NVGcolor fg = contrastTextOn(fill);
            nvgBeginPath(vg);
            nvgMoveTo(vg, bx + boxSize * 0.25f, by + boxSize * 0.52f);
            nvgLineTo(vg, bx + boxSize * 0.44f, by + boxSize * 0.70f);
            nvgLineTo(vg, bx + boxSize * 0.76f, by + boxSize * 0.32f);
            nvgStrokeColor(vg, fg);
            nvgStrokeWidth(vg, 2.5f);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_ROUND);
            nvgStroke(vg);
        } else if (state_ == State::Partial) {
            NVGcolor fill = style::stateWarn();
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, by, boxSize, boxSize, r);
            nvgFillColor(vg, fill);
            nvgFill(vg);

            NVGcolor fg = contrastTextOn(fill);
            nvgBeginPath(vg);
            nvgMoveTo(vg, bx + boxSize * 0.27f, by + boxSize * 0.50f);
            nvgLineTo(vg, bx + boxSize * 0.73f, by + boxSize * 0.50f);
            nvgStrokeColor(vg, fg);
            nvgStrokeWidth(vg, 2.5f);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
        } else if (state_ == State::Disabled) {
            NVGcolor border = style::isLight() ? nvgRGBA(160, 170, 184, 150) : nvgRGBA(255, 255, 255, 50);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx + 1.0f, by + 1.0f, boxSize - 2.0f, boxSize - 2.0f, r);
            nvgStrokeColor(vg, border);
            nvgStrokeWidth(vg, 1.8f);
            nvgStroke(vg);
        } else {
            NVGcolor border = style::isLight() ? nvgRGBA(120, 132, 148, 220) : nvgRGBA(255, 255, 255, 110);
            NVGcolor bg = style::isLight() ? nvgRGBA(255, 255, 255, 140) : nvgRGBA(255, 255, 255, 10);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx + 1.0f, by + 1.0f, boxSize - 2.0f, boxSize - 2.0f, r);
            nvgFillColor(vg, bg);
            nvgFill(vg);
            nvgStrokeColor(vg, border);
            nvgStrokeWidth(vg, 1.8f);
            nvgStroke(vg);
        }
    }

private:
    float size_;
    State state_ = State::Unchecked;
};

class ToggleSwitchView : public brls::View {
public:
    ToggleSwitchView() {
        setWidth(40.0f);
        setHeight(22.0f);
        setFocusable(false);
    }

    void setOn(bool on) { on_ = on; }
    bool isOn() const { return on_; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        const float w = 40.0f;
        const float h = 22.0f;
        const float sx = x + (width - w) * 0.5f;
        const float sy = y + (height - h) * 0.5f;
        const float r = h * 0.5f;

        NVGcolor trackColor = on_
            ? ThemeManager::instance().getAccentColor()
            : (style::isLight() ? nvgRGBA(176, 186, 200, 220) : nvgRGBA(255, 255, 255, 56));
        nvgBeginPath(vg);
        nvgRoundedRect(vg, sx, sy, w, h, r);
        nvgFillColor(vg, trackColor);
        nvgFill(vg);

        const float knobR = 8.0f;
        const float knobCx = on_ ? (sx + w - 3.0f - knobR) : (sx + 3.0f + knobR);
        const float knobCy = sy + h * 0.5f;
        NVGcolor knobColor = on_ ? contrastTextOn(trackColor) : nvgRGB(255, 255, 255);
        nvgBeginPath(vg);
        nvgCircle(vg, knobCx, knobCy, knobR);
        nvgFillColor(vg, knobColor);
        nvgFill(vg);
    }

private:
    bool on_ = false;
};

class StorageBarView : public brls::View {
public:
    StorageBarView() {
        setHeight(12.0f);
        setFocusable(false);
    }

    void setValues(int64_t totalBytes, int64_t freeBytes, uint64_t neededBytes) {
        totalBytes_ = totalBytes;
        freeBytes_ = freeBytes;
        neededBytes_ = neededBytes;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (width <= 2.0f) return;
        const float h = std::min(height, 12.0f);
        const float by = y + (height - h) * 0.5f;
        const float r = h * 0.5f;

        // Track (represents total capacity)
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, by, width, h, r);
        nvgFillColor(vg, style::barTrack());
        nvgFill(vg);

        if (totalBytes_ <= 0) return;

        double total = static_cast<double>(totalBytes_);
        double freeB = std::max(0.0, static_cast<double>(freeBytes_));
        double usedB = std::max(0.0, total - freeB);
        double needB = static_cast<double>(neededBytes_);

        float usedRatio = static_cast<float>(std::clamp(usedB / total, 0.0, 1.0));
        float usedW = width * usedRatio;
        if (usedB > 0.0 && usedW < 4.0f) usedW = 4.0f;

        nvgSave(vg);
        nvgScissor(vg, x, by, width, h);

        // Segment 1: already used on drive
        if (usedW > 0.5f) {
            NVGcolor usedColor = style::isLight() ? nvgRGBA(130, 142, 158, 175) : nvgRGBA(255, 255, 255, 80);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, by, usedW, h, r);
            nvgFillColor(vg, usedColor);
            nvgFill(vg);
        }

        // Segment 2: selected download
        if (needB > 0.0) {
            bool over = (needB > freeB);
            float needRatio = static_cast<float>(std::clamp(needB / total, 0.0, 1.0));
            float needW = width * needRatio;
            if (needW < 6.0f) needW = 6.0f;

            float segStart = usedW;
            if (segStart + needW > width) {
                needW = std::max(4.0f, width - segStart);
            }
            if (over && usedW >= width - 4.0f) {
                segStart = std::max(0.0f, width - 10.0f);
                needW = 10.0f;
            }

            NVGcolor needColor;
            if (over) {
                needColor = style::stateError();
            } else if ((usedB + needB) / total >= 0.90) {
                needColor = style::stateWarn();
            } else {
                needColor = ThemeManager::instance().getAccentColor();
            }

            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + segStart, by, needW, h, r);
            nvgFillColor(vg, needColor);
            nvgFill(vg);
        }

        nvgRestore(vg);
    }

private:
    int64_t totalBytes_ = 0;
    int64_t freeBytes_ = 0;
    uint64_t neededBytes_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool isRomFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> romExts = {
        ".zip", ".7z", ".rar", ".iso", ".chd", ".cso", ".bin", ".cue", ".img",
        ".nes", ".fds", ".unf", ".smc", ".sfc", ".fig", ".swc", ".z64", ".n64", ".v64",
        ".gba", ".gbc", ".gb", ".nds", ".dsi", ".3ds", ".3dsx", ".cia",
        ".gcm", ".gcz", ".wbfs", ".wad", ".rpx", ".wud", ".wux",
        ".pbp", ".vpk", ".smd", ".gen", ".md", ".gg", ".sg", ".sms", ".cdi", ".gdi",
        ".pce", ".sgx", ".ngp", ".ngc", ".ws", ".wsc", ".col", ".vec", ".a26", ".a52", ".a78", ".lnx"
    };
    for (const auto& ext : romExts) {
        if (lower.size() >= ext.size() && lower.rfind(ext) == lower.size() - ext.size()) {
            return true;
        }
    }
    return false;
}

static bool isPlayableFile(const std::string& filename) {
    if (isRomFile(filename)) return true;
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 &&
           (lower.rfind(".nro") == lower.size() - 4 ||
            lower.rfind(".ovl") == lower.size() - 4 ||
            lower.rfind(".elf") == lower.size() - 4);
}

static bool isMediaOrMetaPath(const std::string& fullpath) {
    std::string lower = fullpath;
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::vector<std::string> mediaPatterns = {
        "/images/", "/image/", "/covers/", "/cover/", "/screenshots/", "/screenshot/",
        "/snaps/", "/titles/", "/boxart/", "/boxarts/", "/manuals/", "/manual/",
        "/cheats/", "/cheat/", "/saves/", "/save/", "/wheel/", "/wheels/",
        "images/", "image/", "covers/", "cover/", "screenshots/", "screenshot/",
        "snaps/", "titles/", "boxart/", "boxarts/", "manuals/", "manual/",
        "cheats/", "cheat/", "saves/", "save/", "wheel/", "wheels/"
    };
    for (const auto& pat : mediaPatterns) {
        if (pat.front() == '/') {
            if (lower.find(pat) != std::string::npos) return true;
        } else {
            if (lower.rfind(pat, 0) == 0) return true;
        }
    }
    return false;
}

static std::string computeBaseFolder(const std::vector<torrent::TorrentFileInfo>& files) {
    if (files.empty()) return "";
    std::string first = files[0].name;
    std::replace(first.begin(), first.end(), '\\', '/');
    size_t slash = first.find('/');
    if (slash == std::string::npos) return "";
    std::string root = first.substr(0, slash);
    if (root.empty()) return "";

    for (size_t i = 1; i < files.size(); ++i) {
        std::string p = files[i].name;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (p.rfind(root + "/", 0) != 0) {
            return "";
        }
    }
    return root;
}

static bool isSwitchGameFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 &&
           (lower.rfind(".nsp") == lower.size() - 4 ||
            lower.rfind(".nsz") == lower.size() - 4 ||
            lower.rfind(".xci") == lower.size() - 4 ||
            lower.rfind(".xcz") == lower.size() - 4);
}

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) {
                parts.push_back(part);
                part.clear();
            }
        } else {
            part += c;
        }
    }
    if (!part.empty()) {
        parts.push_back(part);
    }
    return parts;
}

static std::string baseFileNameOnly(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

static std::string fileExtUpper(const std::string& path) {
    std::string name = baseFileNameOnly(path);
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

static std::string cleanStemForMultiGame(const std::string& path) {
    std::string s = baseFileNameOnly(path);
    size_t dot = s.rfind('.');
    if (dot != std::string::npos) s.erase(dot);
    size_t bracket = s.find('[');
    if (bracket != std::string::npos && bracket > 0) {
        s.erase(bracket);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '_' || s.back() == '-')) {
        s.pop_back();
    }
    std::replace(s.begin(), s.end(), '_', ' ');
    return s.empty() ? baseFileNameOnly(path) : s;
}

static brls::Box* makeBadgePill(const std::string& text, NVGcolor bg, NVGcolor fg, float marginRight = 6.0f) {
    auto* box = new brls::Box();
    box->setPadding(2.0f, 7.0f, 2.0f, 7.0f);
    box->setCornerRadius(style::kRadiusChip);
    box->setBackgroundColor(bg);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setShrink(0.0f);
    if (marginRight > 0.0f) box->setMarginRight(marginRight);

    auto* lbl = new brls::Label();
    lbl->setText(text);
    lbl->setFontSize(13.0f);
    lbl->setTextColor(fg);
    lbl->setSingleLine(true);
    box->addView(lbl);
    return box;
}

#ifdef __SWITCH__
#include <switch.h>

struct SwitchServiceGuard {
    bool ns_ok = false;
    bool ncm_ok = false;
    NcmContentMetaDatabase db_sd = {};
    bool db_sd_open = false;
    NcmContentMetaDatabase db_nand = {};
    bool db_nand_open = false;

    NcmContentStorage cs_sd = {};
    bool cs_sd_open = false;
    NcmContentStorage cs_nand = {};
    bool cs_nand_open = false;

    SwitchServiceGuard() {
        g_switch_service_mutex.lock();
        ns_ok = R_SUCCEEDED(nsInitialize());
        ncm_ok = R_SUCCEEDED(ncmInitialize());
        if (ncm_ok) {
            db_sd_open = R_SUCCEEDED(ncmOpenContentMetaDatabase(&db_sd, NcmStorageId_SdCard));
            db_nand_open = R_SUCCEEDED(ncmOpenContentMetaDatabase(&db_nand, NcmStorageId_BuiltInUser));
            cs_sd_open = R_SUCCEEDED(ncmOpenContentStorage(&cs_sd, NcmStorageId_SdCard));
            cs_nand_open = R_SUCCEEDED(ncmOpenContentStorage(&cs_nand, NcmStorageId_BuiltInUser));
        }
        util::logLine("FileSelectView: SwitchServiceGuard init: ns=" + std::to_string(ns_ok) +
                      " ncm=" + std::to_string(ncm_ok) +
                      " db_sd=" + std::to_string(db_sd_open) +
                      " db_nand=" + std::to_string(db_nand_open) +
                      " cs_sd=" + std::to_string(cs_sd_open) +
                      " cs_nand=" + std::to_string(cs_nand_open));
    }

    ~SwitchServiceGuard() {
        if (cs_sd_open) ncmContentStorageClose(&cs_sd);
        if (cs_nand_open) ncmContentStorageClose(&cs_nand);
        if (db_sd_open) ncmContentMetaDatabaseClose(&db_sd);
        if (db_nand_open) ncmContentMetaDatabaseClose(&db_nand);
        if (ncm_ok) ncmExit();
        if (ns_ok) nsExit();
        g_switch_service_mutex.unlock();
    }

    bool hasContentId(const NcmContentId& cid) {
        bool has = false;
        if (cs_sd_open && R_SUCCEEDED(ncmContentStorageHas(&cs_sd, &has, &cid)) && has) return true;
        if (cs_nand_open && R_SUCCEEDED(ncmContentStorageHas(&cs_nand, &has, &cid)) && has) return true;
        return false;
    }

    bool verifyContentPresent(NcmContentMetaDatabase* db, const NcmContentMetaKey& key) {
        if (!db) return false;
        static const NcmContentType types[] = {
            NcmContentType_Program,
            NcmContentType_Data,
            NcmContentType_Control,
            NcmContentType_HtmlDocument,
            NcmContentType_LegalInformation
        };

        bool foundAnyContent = false;
        for (NcmContentType ct : types) {
            NcmContentId cid;
            if (R_SUCCEEDED(ncmContentMetaDatabaseGetContentIdByType(db, &cid, &key, ct))) {
                foundAnyContent = true;
                if (hasContentId(cid)) return true;
            }
        }

        // AddOnContent / DLC can be metadata-only (license / unlock) with 0 content NCAs
        if (!foundAnyContent && key.type == NcmContentMetaType_AddOnContent) {
            return true;
        }

        return false;
    }

    // Highest installed version of `tid` (SD or NAND), 0 when not installed.
    uint32_t installedVersion(uint64_t tid) {
        uint32_t best = 0;
        NcmContentMetaKey key = {};
        if (db_sd_open && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db_sd, &key, tid))) {
            best = key.version;
        }
        if (db_nand_open && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db_nand, &key, tid))) {
            if (key.version > best) best = key.version;
        }
        return best;
    }

    bool isInstalled(uint64_t tid, uint32_t file_version = 0) {
        if (tid == 0) return false;

        NcmContentMetaKey key = {};
        // 1) Try SD card meta database
        if (db_sd_open && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db_sd, &key, tid))) {
            if (verifyContentPresent(&db_sd, key)) {
                if ((tid & 0xFFFULL) == 0x800 && file_version > 0) {
                    return key.version >= file_version;
                }
                return true;
            }
        }

        // 2) Try NAND meta database
        if (db_nand_open && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db_nand, &key, tid))) {
            if (verifyContentPresent(&db_nand, key)) {
                if ((tid & 0xFFFULL) == 0x800 && file_version > 0) {
                    return key.version >= file_version;
                }
                return true;
            }
        }

        return false;
    }
};

static inline bool isTitleIdInstalled(uint64_t tid, SwitchServiceGuard& guard, uint32_t file_version = 0) {
    return guard.isInstalled(tid, file_version);
}
#else
struct SwitchServiceGuard {
    uint32_t installedVersion(uint64_t tid) { (void)tid; return 0; }
    bool isInstalled(uint64_t tid, uint32_t file_version = 0) {
        (void)file_version;
        if (tid == 0x01000BF0152FB131ULL) return true; // DLC Left 4 Dead Bundle (mock)
        return false;
    }
};

static inline bool isTitleIdInstalled(uint64_t tid, SwitchServiceGuard& guard, uint32_t file_version = 0) {
    return guard.isInstalled(tid, file_version);
}
#endif

static uint64_t resolveFileTitleId(const std::string& filename, const std::string& game_title_id) {
    uint64_t tid = util::parseTitleIdFromFileName(filename);
    if (tid != 0) return tid;

    // Fallback: if filename didn't contain a Title ID in brackets/name, but game_title_id is set
    // and this file is a Switch base game (not marked as update or dlc), use game_title_id.
    if (!game_title_id.empty() && isSwitchGameFile(filename)) {
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool is_update = (lower.find("upd") != std::string::npos || lower.find("patch") != std::string::npos);
        bool is_dlc = (lower.find("dlc") != std::string::npos || lower.find("addon") != std::string::npos);
        if (!is_update && !is_dlc) {
            try {
                uint64_t g_tid = std::stoull(game_title_id, nullptr, 16);
                if (util::isSwitchTitleId(g_tid)) {
                    return g_tid;
                }
            } catch (...) {}
        }
    }
    return 0;
}

bool isSwitchPackageFile(const std::string& filename) {
    return isSwitchGameFile(filename);
}

uint64_t torrentFileTitleId(const std::string& filename, const std::string& gameTitleId) {
    return resolveFileTitleId(filename, gameTitleId);
}

void queueSwitchPackage(const Game& game, const torrent::TorrentFileInfo& file, bool no_install) {
    std::vector<int> singleSelected = { file.index };
    Game singleGame = game;
    singleGame.title = cleanTitle(game.title);
    singleGame.topic_id = game.topic_id + "_" + std::to_string(file.index);

    auto comp = util::classifyDownloadComponent(file.name, game.title);
    unsigned long long pkgSize = (file.size > 0) ? static_cast<unsigned long long>(file.size) : 0;
    ui::DownloadManager::instance().addDownload(
        singleGame, singleSelected, file.index, file.name, "",
        game.topic_id, cleanTitle(game.title), comp.label, comp.cleanName, comp.type, pkgSize, no_install);
}

namespace {

// This release against the newest release of the game in the catalog and
// the installed game.
struct ReleaseUpdateCheck {
    bool warn = false;
    std::string inRelease;      // version of this release
    std::string newest;         // version of the newest catalog release
    std::string installed;      // installed version, "" = not installed
    std::vector<Game> others;   // other catalog releases of the game, newest first
    uint64_t appId = 0;         // the game
};

// Runs on a worker: the ncm query is too slow for the UI thread.
ReleaseUpdateCheck checkReleaseUpdate(const Game& game) {
    ReleaseUpdateCheck r;
    // Only a single-game release: in a multi-game one the files picked may
    // belong to any of its games.
    auto appIds = catalog::releaseAppIds(game);
    if (appIds.size() != 1) return r;
    const uint64_t appId = appIds.front();
    auto releases = catalog::findReleases(appId);
    if (releases.empty() || releases.front().topic_id == game.topic_id) return r;
    const Game& newest = releases.front();
    const catalog::TitleVersion newestVer = catalog::titleVersion(newest, appId);
    const catalog::TitleVersion thisVer = catalog::titleVersion(game, appId);
    if (catalog::compareRelease(newestVer, thisVer.version, thisVer.update) != catalog::VersionCmp::Newer) {
        return r;
    }
    util::InstalledGameInfo info = util::queryInstalledGame(appId);
    // The newer version is already on the console: nothing to miss
    if (info.installed &&
        catalog::compareRelease(newestVer, info.displayVersion, info.patchVersion) != catalog::VersionCmp::Newer) {
        return r;
    }

    r.warn = true;
    r.appId = appId;
    r.inRelease = catalog::versionLabel(thisVer);
    r.newest = catalog::versionLabel(newestVer);
    if (info.installed) {
        r.installed = catalog::versionLabel(info.displayVersion, info.patchVersion);
        if (r.installed.empty()) r.installed = "app/fileselect/installed_unknown_ver"_i18n;
    }
    for (auto& g : releases) {
        if (g.topic_id != game.topic_id) r.others.push_back(std::move(g));
    }
    util::logLine("FileSelectView: release " + game.topic_id + " has '" + thisVer.version + "', topic " +
                  newest.topic_id + " has '" + newestVer.version + "', installed '" + info.displayVersion +
                  "', other releases: " + std::to_string(r.others.size()));
    return r;
}

void showOldUpdateWarning(const ReleaseUpdateCheck& r) {
    std::string installed = r.installed.empty() ? "app/fileselect/update_none"_i18n : r.installed;
    auto* dialog = new brls::Dialog(brls::getStr("app/fileselect/newer_release_warn", r.inRelease, r.newest, installed));
    if (!r.others.empty()) {
        auto others = r.others;
        const uint64_t appId = r.appId;
        dialog->addButton("app/fileselect/other_releases"_i18n, [others, appId]() {
            showReleaseChooser(others, [](const Game& g) {
                brls::Application::pushActivity(new GameDetailView(g));
            }, appId);
        });
    }
    dialog->addButton("app/common/continue"_i18n, []() {});
    dialog->open();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// FileSelectView
// ─────────────────────────────────────────────────────────────────────────────

std::atomic<bool> g_file_select_view_active{false};

FileSelectView::FileSelectView(const Game& game, const std::string& retro_console_id, bool no_install)
    : game_(game),
      retro_console_id_(retro_console_id),
      noInstall_(no_install),
      alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
    g_file_select_view_active = true;
    // Background cover downloads compete for BSD sockets/sessions with the
    // custom engine probe. Pause them while this view is open.
    net::ImageDownloader::instance().pause();
}

FileSelectView::~FileSelectView() {
    alive_flag_->store(false);
    g_file_select_view_active = false;
    net::ImageDownloader::instance().resume();
    // Abort a still-running probe and drop its torrent (unless a download has
    // adopted it) so the engine does not keep it around forever.
    datasource::CustomEngineClient::instance().cancelProbe();
    datasource::CustomEngineClient::instance().releaseProbeTorrent();
}

void FileSelectView::onContentAvailable() {
    util::logLine("FileSelectView: onContentAvailable start");

    title->setText(cleanTitle(game_.title));

    // Apply glass card styling from UiTokens
    if (leftPanel) {
        leftPanel->setBackgroundColor(style::cardBg());
        leftPanel->setBorderColor(style::cardBorder());
        leftPanel->setBorderThickness(1.0f);
    }
    if (rightPanel) {
        rightPanel->setBackgroundColor(style::cardBg());
        rightPanel->setBorderColor(style::cardBorder());
        rightPanel->setBorderThickness(1.0f);
    }
    if (summaryDivider) {
        summaryDivider->setBackgroundColor(style::cardBorder());
    }
    if (breadcrumbBar) {
        breadcrumbBar->setBackgroundColor(rowCardBg());
        breadcrumbBar->setBorderColor(rowCardBorder());
        breadcrumbBar->setBorderThickness(1.0f);
    }
    if (breadcrumbIcon) breadcrumbIcon->setTextColor(ThemeManager::instance().getAccentColor());
    if (breadcrumbPath) breadcrumbPath->setTextColor(style::textPrimary());
    if (breadcrumbCount) breadcrumbCount->setTextColor(style::textSecondary());

    if (selectedCaption) {
        selectedCaption->setTextColor(style::textSecondary());
        selectedCaption->setLetterSpacing(0.8f);
    }
    if (selectedCountText) {
        selectedCountText->setTextColor(style::textSecondary());
    }
    if (totalSizeText) {
        totalSizeText->setTextColor(ThemeManager::instance().getTextAccentColor());
    }
    if (unpackedSizeText) {
        unpackedSizeText->setTextColor(style::textSecondary());
    }
    if (storageCaption) {
        storageCaption->setTextColor(style::textSecondary());
        storageCaption->setLetterSpacing(0.8f);
    }
    if (secondarySpaceText) {
        secondarySpaceText->setTextColor(style::textSecondary());
    }

    // Mount spinner in loadingStateBox
    if (spinnerSlot) {
        spinner_ = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
        spinner_->setWidth(56.0f);
        spinner_->setHeight(56.0f);
        spinnerSlot->addView(spinner_);
    }
    if (loadingTitle) loadingTitle->setTextColor(style::textPrimary());
    if (loadingStatus) loadingStatus->setTextColor(style::textSecondary());

    // Mount CloudError icon in errorStateBox
    if (errorIconSlot) {
        errorIconSlot->addView(new DashboardIcon(DashboardIcon::Kind::CloudError, 88.0f));
    }
    if (errorTitle) errorTitle->setTextColor(style::textPrimary());
    if (errorMessage) errorMessage->setTextColor(style::textSecondary());

    // Mount ToggleSwitchView and StorageBarView
    if (noInstallSwitchSlot) {
        noInstallSwitch_ = new ToggleSwitchView();
        noInstallSwitch_->setOn(noInstall_);
        noInstallSwitchSlot->addView(noInstallSwitch_);
    }
    if (storageBarSlot) {
        storageBar_ = new StorageBarView();
        storageBar_->setWidthPercentage(100.0f);
        storageBarSlot->addView(storageBar_);
    }

    // Wire fixed target path vs Switch storage controls
    const bool fixedTarget = isFolderHierarchyMode() || isPcGame(game_);
    if (fixedTarget) {
        if (storageCaption) storageCaption->setText("app/fileselect/section_save_folder"_i18n);
        if (storageChipsRow) storageChipsRow->setVisibility(brls::Visibility::GONE);
        if (noInstallToggleRow) noInstallToggleRow->setVisibility(brls::Visibility::GONE);
        if (targetPathBox) {
            targetPathBox->setVisibility(brls::Visibility::VISIBLE);
            targetPathBox->setBackgroundColor(rowCardBg());
            targetPathBox->setBorderColor(rowCardBorder());
            targetPathBox->setBorderThickness(1.0f);
        }
        if (targetPathIcon) targetPathIcon->setTextColor(ThemeManager::instance().getAccentColor());
        if (targetPathText) {
            if (!retro_console_id_.empty()) {
                targetPathText->setText(catalog::RetroCatalogManager::instance().getTargetDir(retro_console_id_));
            } else if (isPcGame(game_)) {
                targetPathText->setText(TSNX_PC_GAMES_DIR);
            } else {
                targetPathText->setText(TSNX_DOWNLOADS_DIR);
            }
            targetPathText->setTextColor(style::textPrimary());
        }
    } else {
        if (storageCaption) storageCaption->setText("app/fileselect/section_storage"_i18n);
        if (storageChipsRow) storageChipsRow->setVisibility(brls::Visibility::VISIBLE);
        if (noInstallToggleRow) {
            noInstallToggleRow->setVisibility(brls::Visibility::VISIBLE);
            noInstallToggleRow->registerClickAction([this](brls::View*) {
                toggleNoInstallMode();
                return true;
            });
            noInstallToggleRow->getFocusEvent()->subscribe([this](bool) {
                updateStorageChipsUi();
            });
        }
        if (targetPathBox) targetPathBox->setVisibility(brls::Visibility::GONE);

        auto bindChip = [this](brls::Box* chip, const std::string& loc) {
            if (!chip) return;
            chip->registerClickAction([this, loc, chip](brls::View*) {
                selectStorageLocation(loc, chip);
                return true;
            });
        };
        bindChip(chipAuto, "auto");
        bindChip(chipSd, "sd");
        bindChip(chipNand, "nand");
        bindChip(chipUsb, "usb");
    }

    // Wire action buttons
    if (btnRetry) {
        btnRetry->registerClickAction([this](brls::View*) {
            if (!probeInFlight_) {
                startProbe();
            }
            return true;
        });
    }
    if (btnDownloadAll) {
        btnDownloadAll->registerClickAction([this](brls::View*) {
            startDownloadAndGoToDownloads();
            return true;
        });
    }
    if (btnDownload) {
        btnDownload->registerClickAction([this](brls::View*) {
            startDownloadAndGoToDownloads();
            return true;
        });
    }
    if (btnToggleAll) {
        btnToggleAll->registerClickAction([this](brls::View*) {
            toggleAllSelection();
            return true;
        });
    }

    // Activity controller shortcuts
    this->registerAction("app/fileselect/select_all"_i18n, brls::ControllerButton::BUTTON_X,
        [this](brls::View*) { toggleAllSelection(); return true; }, false);

    this->registerAction("app/fileselect/download_btn"_i18n, brls::ControllerButton::BUTTON_START,
        [this](brls::View*) { startDownloadAndGoToDownloads(); return true; });

    this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        if (isFolderHierarchyMode() && currentFolder_ != baseFolder_) {
            navigateUp();
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_BACKSPACE), [this](brls::View*) {
        if (isFolderHierarchyMode() && currentFolder_ != baseFolder_) {
            navigateUp();
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    this->registerAction("app/actions/refresh"_i18n, brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        if (files_.empty() && !probeInFlight_) {
            startProbe();
            return true;
        }
        return false;
    }, true);

    setLeftState(LeftState::Loading);
    updateStorageChipsUi();
    refreshStorageStats();
    updateSummaryUi();
    updateHeaderMeta();
    updateCrossColumnRoutes();

    if (loadingStateBox) {
        brls::Application::giveFocus(loadingStateBox);
    }

    startProbe();

    util::logLine("FileSelectView: onContentAvailable end");
}

// ─────────────────────────────────────────────────────────────────────────────
// Left column state & header meta
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::setLeftState(LeftState state) {
    leftState_ = state;

    if (loadingStateBox) {
        loadingStateBox->setVisibility(state == LeftState::Loading ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (errorStateBox) {
        errorStateBox->setVisibility(state == LeftState::Error ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (fileListScroll) {
        fileListScroll->setVisibility(state == LeftState::List ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (breadcrumbBar) {
        const bool showBreadcrumbs = (state == LeftState::List) && isFolderHierarchyMode();
        breadcrumbBar->setVisibility(showBreadcrumbs ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (spinner_) {
        spinner_->animate(state == LeftState::Loading);
    }
}

void FileSelectView::updateHeaderMeta() {
    if (!headerMeta) return;

    if (leftState_ == LeftState::Loading) {
        headerMeta->setText("app/fileselect/probing"_i18n);
        return;
    }

    if (leftState_ == LeftState::Error || files_.empty()) {
        headerMeta->setText(style::headerMeta("", {
            style::buttonHint(brls::ControllerButton::BUTTON_Y, "app/header_hint/refresh"_i18n)
        }));
        return;
    }

    std::string stats = util::pluralize("app/plural/files", static_cast<long long>(files_.size()));
    if (isFolderHierarchyMode()) {
        headerMeta->setText(style::headerMeta(stats, {
            style::buttonHint(brls::ControllerButton::BUTTON_Y, "app/fileselect/toggle_folder"_i18n)
        }));
    } else {
        headerMeta->setText(stats);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Probe status polling & startProbe
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::scheduleProbeStatusPoll(std::shared_ptr<std::atomic<bool>> alive,
                                             std::shared_ptr<std::atomic<bool>> running) {
    brls::delay(1000, [this, alive, running]() {
        if (!alive->load() || !running->load() || g_appExiting.load()) return;
        brls::async([this, alive, running]() {
            auto status = datasource::CustomEngineClient::instance().probeStatus();
            brls::sync([this, status, alive, running]() {
                if (!alive->load() || !running->load() || g_appExiting.load()) return;
                std::string detail;
                if (status.active && status.meta_peers_total > 0) {
                    detail = brls::getStr("app/fileselect/meta_peers",
                                          std::to_string(status.meta_peers_tried),
                                          std::to_string(status.meta_peers_total));
                } else if (status.active) {
                    detail = brls::getStr("app/fileselect/peer_search", status.phase);
                } else {
                    detail = brls::getStr("app/fileselect/swarm_stats",
                                          std::to_string(status.seeds),
                                          std::to_string(status.peers),
                                          std::to_string(status.dht_nodes));
                }
                // Strip outer parentheses if the translation includes them
                if (detail.size() >= 2 && detail.front() == '(' && detail.back() == ')') {
                    detail = detail.substr(1, detail.size() - 2);
                }
                if (loadingStatus) {
                    loadingStatus->setText(detail);
                }
                scheduleProbeStatusPoll(alive, running);
            });
        });
    });
}

void FileSelectView::startProbe() {
    probeInFlight_ = true;
    setLeftState(LeftState::Loading);
    if (loadingStatus) {
        loadingStatus->setText("app/fileselect/probing"_i18n);
    }
    updateHeaderMeta();
    updateSummaryUi();
    updateCrossColumnRoutes();
    if (loadingStateBox) {
        brls::Application::giveFocus(loadingStateBox);
    }

    auto alive = alive_flag_;
    auto status_running = std::make_shared<std::atomic<bool>>(true);
    scheduleProbeStatusPoll(alive, status_running);

    Game gameCopy = game_;
    bool switchMode = !isFolderHierarchyMode();
    brls::async([this, status_running, alive, gameCopy, switchMode]() {
        util::logLine("FileSelectView: probe thread started");
        std::vector<torrent::TorrentFileInfo> probedFiles;
        std::string err;

        util::logLine("FileSelectView: probing magnet " + gameCopy.magnet);
        bool success = ui::DownloadManager::instance().getImpl()
                           .probeTorrentFiles(gameCopy.magnet, probedFiles, &err);
        util::logLine("FileSelectView: probe done, success=" + std::to_string(success) +
                      " count=" + std::to_string(probedFiles.size()));

        status_running->store(false);

        // Is there a newer release of the game in the catalog? Checked here,
        // on the worker: it queries ncm.
        auto updateCheck = std::make_shared<ReleaseUpdateCheck>();
        if (success && switchMode && alive->load()) {
            *updateCheck = checkReleaseUpdate(gameCopy);
        }

        brls::sync([this, success, probedFiles, err, alive, updateCheck]() {
            if (!alive->load()) {
                util::logLine("FileSelectView: view already destroyed, aborting sync");
                return;
            }
            probeInFlight_ = false;
            util::logLine("FileSelectView: sync callback, success=" +
                          std::to_string(success) + " count=" +
                          std::to_string(probedFiles.size()));

            if (success && !probedFiles.empty()) {
                files_ = probedFiles;
                installedCache_.clear();
                installedExpanded_ = false;
                selected_.assign(files_.size(), false);
                if (isFolderHierarchyMode()) {
                    baseFolder_ = computeBaseFolder(files_);
                    currentFolder_ = baseFolder_;
                    bool anySelected = false;
                    for (size_t i = 0; i < files_.size(); ++i) {
                        bool isPrimary = !retro_console_id_.empty()
                            ? isRomFile(files_[i].name)
                            : !isMediaOrMetaPath(files_[i].name);
                        if (isPrimary && !isMediaOrMetaPath(files_[i].name)) {
                            selected_[i] = true;
                            anySelected = true;
                        }
                    }
                    if (!anySelected) {
                        for (size_t i = 0; i < files_.size(); ++i) {
                            if (!isMediaOrMetaPath(files_[i].name)) {
                                selected_[i] = true;
                                anySelected = true;
                            }
                        }
                    }
                    if (!anySelected) {
                        selected_.assign(files_.size(), true);
                    }
                } else {
                    SwitchServiceGuard guard;
                    const bool isFat = util::isSdCardFat32();
                    bool anySelected = false;
                    for (size_t i = 0; i < files_.size(); ++i) {
                        uint64_t tid = resolveFileTitleId(files_[i].name, game_.title_id);
                        uint32_t ver = util::parseVersionFromFileName(files_[i].name);
                        bool isGame = isSwitchGameFile(files_[i].name);
                        bool installed = isTitleIdInstalled(tid, guard, ver);
                        bool tooLarge = noInstall_ && isFat && (static_cast<uint64_t>(files_[i].size) > util::kFat32MaxFileSize);
                        if (isGame && !tooLarge) {
                            if (!noInstall_ && !installed) {
                                selected_[i] = true;
                                anySelected = true;
                            } else if (noInstall_) {
                                selected_[i] = true;
                                anySelected = true;
                            }
                        }
                    }
                    if (noInstall_ && !anySelected && isFat) {
                        brls::Application::notify("app/fileselect/fat32_all_files_too_large"_i18n);
                    }
                }
                setLeftState(LeftState::List);
                calculateUncompressedSizes();
                rebuildFileList();
                refreshStorageStats();
                updateSummaryUi();
                updateHeaderMeta();

                util::logLine("FileSelectView: rebuildFileList done, rows=" +
                              std::to_string(files_.size()));

                if (fileListScroll) fileListScroll->setFocusable(false);

                brls::View* focusTarget = getLeftPanelFallbackView();
                if (focusTarget) {
                    brls::Application::giveFocus(focusTarget);
                }

                if (updateCheck->warn) {
                    showOldUpdateWarning(*updateCheck);
                }
            } else {
                setLeftState(LeftState::Error);
                if (errorMessage) {
                    errorMessage->setText(err.empty() ? "app/fileselect/timeout_error"_i18n : err);
                }
                updateHeaderMeta();
                updateSummaryUi();
                updateCrossColumnRoutes();
                if (btnRetry) {
                    brls::Application::giveFocus(btnRetry);
                }
                brls::Application::notify("app/common/error"_i18n + ": " +
                    (err.empty() ? "app/fileselect/timeout_error"_i18n : err));
                util::logLine("FileSelectView: probe error: " + err);
            }
        });
    });
}

brls::View* FileSelectView::create() { return nullptr; }

// ─────────────────────────────────────────────────────────────────────────────
// rebuildFileList — build the Box children from files_/selected_
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::rebuildFileList(bool keepRightFocus) {
    if (isFolderHierarchyMode()) {
        rebuildRetroFolderList(keepRightFocus);
    } else {
        rebuildSwitchFileList(keepRightFocus);
    }
}

void FileSelectView::navigateToFolder(const std::string& folder) {
    currentFolder_ = folder;
    focusedKey_.clear();
    rebuildRetroFolderList();
    if (brls::View* first = getLeftPanelFallbackView()) {
        brls::Application::giveFocus(first);
    }
}

void FileSelectView::navigateUp() {
    if (currentFolder_.empty() || currentFolder_ == baseFolder_) {
        return;
    }
    size_t lastSlash = currentFolder_.rfind('/');
    if (lastSlash == std::string::npos) {
        currentFolder_ = "";
    } else {
        currentFolder_ = currentFolder_.substr(0, lastSlash);
    }
    focusedKey_.clear();
    rebuildRetroFolderList();
    if (brls::View* first = getLeftPanelFallbackView()) {
        brls::Application::giveFocus(first);
    }
}

void FileSelectView::updateBreadcrumb(size_t folderCount, size_t fileCount) {
    std::string displayPath;
    if (currentFolder_.empty() || currentFolder_ == baseFolder_) {
        displayPath = "/";
    } else if (!baseFolder_.empty() && currentFolder_.rfind(baseFolder_ + "/", 0) == 0) {
        displayPath = "/ \u203A " + currentFolder_.substr(baseFolder_.length() + 1);
    } else {
        displayPath = "/ \u203A " + currentFolder_;
    }

    // Replace '/' inside relative path with breadcrumb separator
    if (displayPath.size() > 4) {
        std::string tail = displayPath.substr(4);
        std::string pretty;
        for (char c : tail) {
            if (c == '/') pretty += " \u203A ";
            else pretty += c;
        }
        displayPath = "/ \u203A " + pretty;
    }

    if (breadcrumbPath) {
        breadcrumbPath->setText(displayPath);
    }
    if (breadcrumbCount) {
        const long long totalItems = static_cast<long long>(folderCount + fileCount);
        breadcrumbCount->setText(util::pluralize("app/plural/files", totalItems));
    }
}

void FileSelectView::rebuildRetroFolderList(bool keepRightFocus) {
    if (!fileListBox) return;
    // Clear right-panel LEFT route and bindings BEFORE clearing views
    bindRightPanelLeftRoute(nullptr);
    lastFocusedListRow_ = nullptr;
    fileRowBindings_.clear();
    fileRowBindings_.resize(files_.size());
    folderRowBindings_.clear();
    fileListBox->clearViews();

    struct SubfolderInfo {
        std::string name;
        std::string fullPath;
        std::vector<size_t> fileIndices;
        uint64_t totalSize = 0;
        size_t selectedCount = 0;
    };

    struct FileEntryInfo {
        std::string name;
        size_t fileIndex = 0;
    };

    std::map<std::string, SubfolderInfo> subfoldersMap;
    std::vector<FileEntryInfo> directFiles;

    std::string prefix = currentFolder_.empty() ? "" : (currentFolder_ + "/");

    for (size_t i = 0; i < files_.size(); ++i) {
        std::string normPath = files_[i].name;
        std::replace(normPath.begin(), normPath.end(), '\\', '/');

        if (!prefix.empty()) {
            if (normPath.rfind(prefix, 0) != 0) {
                continue;
            }
        }

        std::string relPath = normPath.substr(prefix.size());
        if (relPath.empty()) continue;

        size_t slashPos = relPath.find('/');
        if (slashPos != std::string::npos) {
            std::string subName = relPath.substr(0, slashPos);
            std::string subFullPath = prefix + subName;

            auto& sInfo = subfoldersMap[subName];
            if (sInfo.name.empty()) {
                sInfo.name = subName;
                sInfo.fullPath = subFullPath;
            }
            sInfo.fileIndices.push_back(i);
            sInfo.totalSize += files_[i].size;
            if (i < selected_.size() && selected_[i]) {
                sInfo.selectedCount++;
            }
        } else {
            directFiles.push_back({relPath, i});
        }
    }

    std::sort(directFiles.begin(), directFiles.end(), [](const FileEntryInfo& a, const FileEntryInfo& b) {
        return a.name < b.name;
    });

    updateBreadcrumb(subfoldersMap.size(), directFiles.size());

    auto styleCompactRow = [this](brls::Box* row) {
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(54.0f);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(16.0f);
        row->setMarginBottom(6.0f);
        row->setCornerRadius(10.0f);
        row->setBackgroundColor(rowCardBg());
        row->setBorderColor(rowCardBorder());
        row->setBorderThickness(1.0f);
        row->setFocusable(true);
        row->getFocusEvent()->subscribe([this, row](bool focused) {
            if (focused) {
                lastFocusedListRow_ = row;
                bindRightPanelLeftRoute(row);
            }
        });
    };

    // 1. Parent folder row ".." if not at baseFolder_
    if (!currentFolder_.empty() && currentFolder_ != baseFolder_) {
        auto* row = new brls::Box();
        styleCompactRow(row);

        auto* icon = new brls::Label();
        icon->setFontSize(22.0f);
        icon->setText("\uE5D8"); // Arrow up
        icon->setTextColor(ThemeManager::instance().getAccentColor());
        icon->setWidth(32.0f);
        icon->setMarginRight(10.0f);
        row->addView(icon);

        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setShrink(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16.0f);
        nameLbl->setSingleLine(true);
        nameLbl->setText("app/fileselect/parent_folder"_i18n);
        nameLbl->setTextColor(style::textPrimary());
        row->addView(nameLbl);

        row->registerClickAction([this](brls::View*) {
            navigateUp();
            return true;
        });

        fileListBox->addView(row);
    }

    // 2. Subfolders
    for (auto& pair : subfoldersMap) {
        auto& sInfo = pair.second;
        bool allSel = (!sInfo.fileIndices.empty() && sInfo.selectedCount == sInfo.fileIndices.size());
        bool partialSel = (sInfo.selectedCount > 0 && !allSel);

        auto* row = new brls::Box();
        styleCompactRow(row);

        auto* chk = new VectorCheckboxView(22.0f);
        chk->setState(allSel ? VectorCheckboxView::State::Checked
                             : (partialSel ? VectorCheckboxView::State::Partial : VectorCheckboxView::State::Unchecked));
        chk->setMarginRight(12.0f);
        row->addView(chk);

        auto* icon = new brls::Label();
        icon->setFontSize(22.0f);
        icon->setText("\uE2C7"); // Material folder
        icon->setTextColor(ThemeManager::instance().getAccentColor());
        icon->setMarginRight(10.0f);
        row->addView(icon);

        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setShrink(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16.0f);
        nameLbl->setSingleLine(true);
        nameLbl->setText(sInfo.name);
        nameLbl->setTextColor(style::textPrimary());
        nameLbl->setMarginRight(12.0f);
        row->addView(nameLbl);

        auto* infoLbl = new brls::Label();
        infoLbl->setHeight(brls::View::AUTO);
        infoLbl->setFontSize(14.0f);
        infoLbl->setSingleLine(true);
        infoLbl->setText(util::pluralize("app/plural/files", static_cast<long long>(sInfo.fileIndices.size())) +
                         " \u00B7 " + util::formatFileSize(sInfo.totalSize));
        infoLbl->setTextColor(style::textSecondary());
        infoLbl->setMarginRight(8.0f);
        row->addView(infoLbl);

        auto* chevron = new brls::Label();
        chevron->setFontSize(16.0f);
        chevron->setText("\u203A");
        chevron->setTextColor(style::textSecondary());
        row->addView(chevron);

        folderRowBindings_.push_back({row, chk, sInfo.fileIndices});

        row->registerClickAction([this, path = sInfo.fullPath](brls::View*) {
            navigateToFolder(path);
            return true;
        });

        auto toggleFolder = [this, indices = sInfo.fileIndices, chk](brls::View*) {
            bool allSelected = true;
            for (size_t idx : indices) {
                if (idx < selected_.size() && !selected_[idx]) {
                    allSelected = false;
                    break;
                }
            }
            bool newState = !allSelected;
            for (size_t idx : indices) {
                if (idx < selected_.size()) {
                    selected_[idx] = newState;
                }
            }
            chk->setState(newState ? VectorCheckboxView::State::Checked : VectorCheckboxView::State::Unchecked);
            updateSummaryUi();
            updateHeaderMeta();
            return true;
        };
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFolder);

        fileListBox->addView(row);
    }

    // 3. Direct files
    for (const auto& item : directFiles) {
        size_t idx = item.fileIndex;
        const auto& file = files_[idx];
        bool isSel = (idx < selected_.size()) && selected_[idx];

        auto* row = new brls::Box();
        styleCompactRow(row);

        auto* chk = new VectorCheckboxView(22.0f);
        chk->setState(isSel ? VectorCheckboxView::State::Checked : VectorCheckboxView::State::Unchecked);
        chk->setMarginRight(12.0f);
        row->addView(chk);

        auto* icon = new brls::Label();
        icon->setFontSize(20.0f);
        bool isPlayable = isPlayableFile(file.name);
        icon->setText(isPlayable ? "\uE0E0" : "\uE24D");
        icon->setTextColor(isPlayable ? ThemeManager::instance().getAccentColor() : style::textSecondary());
        icon->setMarginRight(10.0f);
        row->addView(icon);

        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setShrink(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16.0f);
        nameLbl->setSingleLine(true);
        nameLbl->setText(item.name);
        nameLbl->setTextColor(style::textPrimary());
        nameLbl->setMarginRight(12.0f);
        row->addView(nameLbl);

        auto* sizeLbl = new brls::Label();
        sizeLbl->setWidth(115.0f);
        sizeLbl->setHeight(brls::View::AUTO);
        sizeLbl->setFontSize(14.0f);
        sizeLbl->setSingleLine(true);
        sizeLbl->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        sizeLbl->setText(util::formatFileSize(file.size));
        sizeLbl->setTextColor(style::textSecondary());
        row->addView(sizeLbl);

        if (idx < fileRowBindings_.size()) {
            fileRowBindings_[idx] = {row, chk, nameLbl, false, false};
        }

        auto toggleFile = [this, idx](brls::View*) {
            if (idx < selected_.size()) {
                selected_[idx] = !selected_[idx];
                updateRowSelectionState(idx);
                updateSummaryUi();
                updateHeaderMeta();
            }
            return true;
        };
        row->registerClickAction(toggleFile);
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFile);

        fileListBox->addView(row);
    }

    if (subfoldersMap.empty() && directFiles.empty()) {
        auto* row = new brls::Box();
        row->setHeight(54.0f);
        row->setPaddingLeft(14.0f);
        row->setAlignItems(brls::AlignItems::CENTER);
        auto* emptyLbl = new brls::Label();
        emptyLbl->setFontSize(16.0f);
        emptyLbl->setText("app/fileselect/no_files"_i18n);
        emptyLbl->setTextColor(style::textSecondary());
        row->addView(emptyLbl);
        fileListBox->addView(row);
    }

    updateCrossColumnRoutes();
    (void)keepRightFocus;
}

void FileSelectView::renderOtherFilesTree(const OtherFileTreeNode& node, int depth, std::map<std::string, brls::View*>& focusMap) {
    // 1. Render Subfolders
    for (const auto& pair : node.subfolders) {
        const auto& sub = pair.second;
        bool isExpanded = (expandedFolders_.count(sub.fullPath) > 0);
        bool allSel = (sub.selectedFiles == sub.totalFiles && sub.totalFiles > 0);
        bool partialSel = (sub.selectedFiles > 0 && !allSel);

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(52.0f);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(12.0f + depth * 22.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(5.0f);
        row->setCornerRadius(10.0f);
        row->setBackgroundColor(rowCardBg());
        row->setBorderColor(rowCardBorder());
        row->setBorderThickness(1.0f);
        row->setFocusable(true);
        row->getFocusEvent()->subscribe([this, row](bool focused) {
            if (focused) {
                lastFocusedListRow_ = row;
                bindRightPanelLeftRoute(row);
            }
        });

        auto* arrow = new brls::Label();
        arrow->setWidth(20.0f);
        arrow->setHeight(brls::View::AUTO);
        arrow->setFontSize(14.0f);
        arrow->setText(isExpanded ? "\u25BE" : "\u25B8");
        arrow->setTextColor(style::textSecondary());
        row->addView(arrow);

        auto* chk = new VectorCheckboxView(22.0f);
        chk->setState(allSel ? VectorCheckboxView::State::Checked
                             : (partialSel ? VectorCheckboxView::State::Partial : VectorCheckboxView::State::Unchecked));
        chk->setMarginRight(10.0f);
        row->addView(chk);

        auto* icon = new brls::Label();
        icon->setFontSize(20.0f);
        icon->setText("\uE2C7");
        icon->setTextColor(ThemeManager::instance().getAccentColor());
        icon->setMarginRight(10.0f);
        row->addView(icon);

        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setShrink(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16.0f);
        nameLbl->setSingleLine(true);
        nameLbl->setText(sub.name);
        nameLbl->setTextColor(style::textPrimary());
        nameLbl->setMarginRight(10.0f);
        row->addView(nameLbl);

        auto* infoLbl = new brls::Label();
        infoLbl->setHeight(brls::View::AUTO);
        infoLbl->setFontSize(14.0f);
        infoLbl->setSingleLine(true);
        infoLbl->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        infoLbl->setText(util::pluralize("app/plural/files", static_cast<long long>(sub.totalFiles)) +
                         " \u00B7 " + util::formatFileSize(sub.size));
        infoLbl->setTextColor(style::textSecondary());
        row->addView(infoLbl);

        folderRowBindings_.push_back({row, chk, sub.allFileIndices});

        std::string rowKey = "folder:" + sub.fullPath;
        focusMap[rowKey] = row;

        // Button A: expand / collapse
        row->registerClickAction([this, path = sub.fullPath](brls::View*) {
            if (expandedFolders_.count(path)) {
                expandedFolders_.erase(path);
            } else {
                expandedFolders_.insert(path);
            }
            focusedKey_ = "folder:" + path;
            rebuildSwitchFileList();
            return true;
        });

        // Button Y: toggle folder selection in-place
        auto toggleFolder = [this, indices = sub.allFileIndices](brls::View*) {
            bool allSelected = true;
            for (size_t idx : indices) {
                if (idx < selected_.size() && !selected_[idx]) {
                    allSelected = false;
                    break;
                }
            }
            bool newState = !allSelected;
            for (size_t idx : indices) {
                if (idx < selected_.size()) {
                    selected_[idx] = newState;
                    updateRowSelectionState(idx);
                }
            }
            refreshFolderCheckboxes();
            updateSummaryUi();
            return true;
        };
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFolder);

        fileListBox->addView(row);

        if (isExpanded) {
            renderOtherFilesTree(sub, depth + 1, focusMap);
        }
    }

    // 2. Render Direct Files
    auto directFilesSorted = node.directFiles;
    std::sort(directFilesSorted.begin(), directFilesSorted.end(), [](const OtherFileTreeNode& a, const OtherFileTreeNode& b) {
        return a.name < b.name;
    });

    for (const auto& item : directFilesSorted) {
        size_t idx = item.fileIndex;
        bool isSel = (idx < selected_.size()) && selected_[idx];

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(52.0f);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(12.0f + depth * 22.0f + 20.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(5.0f);
        row->setCornerRadius(10.0f);
        row->setBackgroundColor(rowCardBg());
        row->setBorderColor(rowCardBorder());
        row->setBorderThickness(1.0f);
        row->setFocusable(true);
        row->getFocusEvent()->subscribe([this, row](bool focused) {
            if (focused) {
                lastFocusedListRow_ = row;
                bindRightPanelLeftRoute(row);
            }
        });

        auto* chk = new VectorCheckboxView(22.0f);
        chk->setState(isSel ? VectorCheckboxView::State::Checked : VectorCheckboxView::State::Unchecked);
        chk->setMarginRight(10.0f);
        row->addView(chk);

        auto* icon = new brls::Label();
        icon->setFontSize(18.0f);
        icon->setText("\uE24D");
        icon->setTextColor(style::textSecondary());
        icon->setMarginRight(10.0f);
        row->addView(icon);

        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setShrink(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16.0f);
        nameLbl->setSingleLine(true);
        nameLbl->setText(item.name);
        nameLbl->setTextColor(style::textPrimary());
        nameLbl->setMarginRight(10.0f);
        row->addView(nameLbl);

        auto* sizeLbl = new brls::Label();
        sizeLbl->setWidth(115.0f);
        sizeLbl->setHeight(brls::View::AUTO);
        sizeLbl->setFontSize(14.0f);
        sizeLbl->setSingleLine(true);
        sizeLbl->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        sizeLbl->setText(util::formatFileSize(item.size));
        sizeLbl->setTextColor(style::textSecondary());
        row->addView(sizeLbl);

        if (idx < fileRowBindings_.size()) {
            fileRowBindings_[idx] = {row, chk, nameLbl, false, false};
        }

        std::string rowKey = "file:" + std::to_string(idx);
        focusMap[rowKey] = row;

        auto toggleFile = [this, idx](brls::View*) {
            if (idx < selected_.size()) {
                selected_[idx] = !selected_[idx];
                updateRowSelectionState(idx);
                refreshFolderCheckboxes();
                updateSummaryUi();
            }
            return true;
        };
        row->registerClickAction(toggleFile);
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFile);

        fileListBox->addView(row);
    }
}

void FileSelectView::rebuildSwitchFileList(bool keepRightFocus) {
    if (!fileListBox) return;

    // Clear right-panel LEFT route and bindings BEFORE clearing views
    bindRightPanelLeftRoute(nullptr);
    lastFocusedListRow_ = nullptr;
    fileRowBindings_.clear();
    fileRowBindings_.resize(files_.size());
    folderRowBindings_.clear();
    fileListBox->clearViews();

    if (installedCache_.size() != files_.size()) {
        SwitchServiceGuard guard;
        installedCache_.assign(files_.size(), 0);
        for (size_t i = 0; i < files_.size(); ++i) {
            if (!isSwitchGameFile(files_[i].name)) continue;
            uint64_t tid = resolveFileTitleId(files_[i].name, game_.title_id);
            uint32_t ver = util::parseVersionFromFileName(files_[i].name);
            installedCache_[i] = isTitleIdInstalled(tid, guard, ver) ? 1 : 0;
        }
    }

    const bool isFat = util::isSdCardFat32();
    const bool light = style::isLight();

    // Count base games across Switch files to detect multi-game compilations
    int baseCount = 0;
    std::vector<size_t> uninstalledIndices;
    std::vector<size_t> installedIndices;
    uint64_t uninstalledBytes = 0;
    uint64_t installedBytes = 0;

    for (size_t i = 0; i < files_.size(); ++i) {
        if (!isSwitchGameFile(files_[i].name)) continue;
        auto comp = util::classifyDownloadComponent(files_[i].name, game_.title);
        if (comp.type == 0) baseCount++;
        if (installedCache_[i]) {
            installedIndices.push_back(i);
            installedBytes += files_[i].size;
        } else {
            uninstalledIndices.push_back(i);
            uninstalledBytes += files_[i].size;
        }
    }
    const bool isMultiGame = (baseCount > 1);

    // Sort Switch packages logically: Base -> Updates -> DLC, then torrent file index
    auto sortSwitchIndices = [this](std::vector<size_t>& vec) {
        std::stable_sort(vec.begin(), vec.end(), [this](size_t a, size_t b) {
            int pa = download::installFilePriority(files_[a].name);
            int pb = download::installFilePriority(files_[b].name);
            if (pa != pb) return pa > pb;
            return files_[a].index < files_[b].index;
        });
    };
    sortSwitchIndices(uninstalledIndices);
    sortSwitchIndices(installedIndices);

    std::map<std::string, brls::View*> focusMap;

    auto addSectionHeader = [this](const std::string& caption, size_t count, uint64_t totalBytes, bool firstSection) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(32.0f);
        row->setWidth(brls::View::AUTO);
        row->setMarginTop(firstSection ? 2.0f : 12.0f);
        row->setMarginBottom(6.0f);
        row->setPaddingLeft(4.0f);
        row->setPaddingRight(4.0f);

        auto* bar = new brls::Box();
        bar->setWidth(4.0f);
        bar->setHeight(16.0f);
        bar->setCornerRadius(2.0f);
        bar->setBackgroundColor(ThemeManager::instance().getAccentColor());
        bar->setMarginRight(10.0f);
        row->addView(bar);

        auto* titleLbl = new brls::Label();
        titleLbl->setFontSize(14.0f);
        titleLbl->setLetterSpacing(0.8f);
        titleLbl->setSingleLine(true);
        titleLbl->setGrow(1.0f);
        titleLbl->setShrink(1.0f);
        titleLbl->setText(caption);
        titleLbl->setTextColor(ThemeManager::instance().getTextAccentColor());
        titleLbl->setMarginRight(10.0f);
        row->addView(titleLbl);

        auto* metaLbl = new brls::Label();
        metaLbl->setFontSize(14.0f);
        metaLbl->setSingleLine(true);
        metaLbl->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        metaLbl->setText(util::pluralize("app/plural/files", static_cast<long long>(count)) +
                         " \u00B7 " + util::formatFileSize(totalBytes));
        metaLbl->setTextColor(style::textSecondary());
        row->addView(metaLbl);

        fileListBox->addView(row);
    };

    auto addSwitchPackageRow = [this, isFat, light, isMultiGame, &focusMap](size_t idx, bool installed, const std::string& keyPrefix) {
        const auto& file = files_[idx];
        const bool tooLarge = noInstall_ && isFat && (static_cast<uint64_t>(file.size) > util::kFat32MaxFileSize);
        if (tooLarge && idx < selected_.size()) {
            selected_[idx] = false;
        }
        const bool isSel = (idx < selected_.size()) && selected_[idx];

        auto comp = util::classifyDownloadComponent(file.name, game_.title);
        std::string typeLabel = "BASE";
        NVGcolor typeBg;
        NVGcolor typeFg;
        std::string displayName;

        if (comp.type == 1) {
            typeLabel = "UPD";
            typeBg = light ? nvgRGB(2, 136, 209) : nvgRGBA(56, 189, 248, 52);
            typeFg = light ? nvgRGB(255, 255, 255) : nvgRGB(125, 211, 252);
            displayName = comp.version.empty()
                ? brls::getStr("app/downloads/comp_patch_plain")
                : brls::getStr("app/downloads/comp_patch", comp.version);
        } else if (comp.type == 2) {
            typeLabel = "DLC";
            typeBg = light ? nvgRGB(230, 126, 34) : nvgRGBA(251, 146, 60, 52);
            typeFg = light ? nvgRGB(255, 255, 255) : nvgRGB(253, 186, 116);
            displayName = comp.cleanName.empty() ? cleanStemForMultiGame(file.name) : comp.cleanName;
        } else {
            typeLabel = "BASE";
            NVGcolor acc = ThemeManager::instance().getAccentColor();
            typeBg = light ? acc : ThemeManager::instance().getAccentMediumColor();
            typeFg = light ? contrastTextOn(acc) : ThemeManager::instance().getTextAccentColor();
            displayName = isMultiGame ? cleanStemForMultiGame(file.name) : brls::getStr("app/downloads/comp_base_game");
        }

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(74.0f);
        row->setWidth(brls::View::AUTO);
        row->setPaddingTop(10.0f);
        row->setPaddingBottom(10.0f);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(16.0f);
        row->setMarginBottom(6.0f);
        row->setCornerRadius(10.0f);
        row->setBackgroundColor(rowCardBg());
        row->setBorderColor(rowCardBorder());
        row->setBorderThickness(1.0f);
        row->setFocusable(true);
        row->getFocusEvent()->subscribe([this, row](bool focused) {
            if (focused) {
                lastFocusedListRow_ = row;
                bindRightPanelLeftRoute(row);
            }
        });

        auto* chk = new VectorCheckboxView(24.0f);
        if (tooLarge) {
            chk->setState(VectorCheckboxView::State::Disabled);
        } else {
            chk->setState(isSel ? VectorCheckboxView::State::Checked : VectorCheckboxView::State::Unchecked);
        }
        chk->setMarginRight(14.0f);
        row->addView(chk);

        // Middle column (2 lines)
        auto* midCol = new brls::Box();
        midCol->setAxis(brls::Axis::COLUMN);
        midCol->setJustifyContent(brls::JustifyContent::CENTER);
        midCol->setGrow(1.0f);
        midCol->setShrink(1.0f);
        midCol->setMarginRight(12.0f);

        // Line 1: [TYPE] + Human-readable title
        auto* topLine = new brls::Box();
        topLine->setAxis(brls::Axis::ROW);
        topLine->setAlignItems(brls::AlignItems::CENTER);
        topLine->setMarginBottom(4.0f);

        topLine->addView(makeBadgePill(typeLabel, typeBg, typeFg, 8.0f));

        auto* titleLbl = new brls::Label();
        titleLbl->setGrow(1.0f);
        titleLbl->setShrink(1.0f);
        titleLbl->setHeight(brls::View::AUTO);
        titleLbl->setFontSize(17.0f);
        titleLbl->setSingleLine(true);
        titleLbl->setText(displayName);
        titleLbl->setTextColor((tooLarge || installed) ? style::textSecondary() : style::textPrimary());
        topLine->addView(titleLbl);
        midCol->addView(topLine);

        // Line 2: Original filename + format chip + installed / FAT32 chip
        auto* botLine = new brls::Box();
        botLine->setAxis(brls::Axis::ROW);
        botLine->setAlignItems(brls::AlignItems::CENTER);

        auto* rawLbl = new brls::Label();
        rawLbl->setShrink(1.0f);
        rawLbl->setHeight(brls::View::AUTO);
        rawLbl->setFontSize(14.0f);
        rawLbl->setSingleLine(true);
        rawLbl->setText(baseFileNameOnly(file.name));
        rawLbl->setTextColor(style::textSecondary());
        rawLbl->setMarginRight(8.0f);
        botLine->addView(rawLbl);

        std::string ext = fileExtUpper(file.name);
        if (!ext.empty()) {
            botLine->addView(makeBadgePill(ext, pillNeutralBg(), style::textSecondary(), (installed || tooLarge) ? 6.0f : 0.0f));
        }
        if (installed) {
            NVGcolor instBg = light ? nvgRGBA(29, 122, 70, 28) : nvgRGBA(110, 220, 150, 32);
            botLine->addView(makeBadgePill("app/fileselect/installed_tag"_i18n, instBg, style::stateOk(), tooLarge ? 6.0f : 0.0f));
        }
        if (tooLarge) {
            NVGcolor errBg = light ? nvgRGBA(196, 43, 28, 28) : nvgRGBA(255, 105, 97, 36);
            botLine->addView(makeBadgePill("app/fileselect/fat32_badge"_i18n, errBg, style::stateError(), 0.0f));
        }
        midCol->addView(botLine);
        row->addView(midCol);

        // Right column: file size
        auto* sizeLbl = new brls::Label();
        sizeLbl->setWidth(110.0f);
        sizeLbl->setHeight(brls::View::AUTO);
        sizeLbl->setFontSize(15.0f);
        sizeLbl->setSingleLine(true);
        sizeLbl->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        sizeLbl->setText(util::formatFileSize(file.size));
        sizeLbl->setTextColor(tooLarge ? style::stateError() : style::textPrimary());
        row->addView(sizeLbl);

        if (idx < fileRowBindings_.size()) {
            fileRowBindings_[idx] = {row, chk, titleLbl, installed, tooLarge};
        }

        focusMap[keyPrefix + std::to_string(idx)] = row;

        row->registerClickAction([this, idx, tooLarge](brls::View*) {
            if (tooLarge) {
                brls::Application::notify("app/fileselect/fat32_file_too_large_notice"_i18n);
                return true;
            }
            if (idx < selected_.size()) {
                selected_[idx] = !selected_[idx];
                updateRowSelectionState(idx);
                updateSummaryUi();
            }
            return true;
        });

        fileListBox->addView(row);
    };

    bool anySectionDrawn = false;

    // 1. Group: NOT INSTALLED (only when non-empty)
    if (!uninstalledIndices.empty()) {
        addSectionHeader("app/fileselect/group_uninstalled"_i18n, uninstalledIndices.size(), uninstalledBytes, !anySectionDrawn);
        anySectionDrawn = true;
        for (size_t idx : uninstalledIndices) {
            addSwitchPackageRow(idx, false, "game:");
        }
    }

    // 2. Group: OTHER FILES (tree view, only when non-empty)
    std::vector<size_t> otherIndices;
    uint64_t otherBytes = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (!isSwitchGameFile(files_[i].name)) {
            otherIndices.push_back(i);
            otherBytes += files_[i].size;
        }
    }
    if (!otherIndices.empty()) {
        addSectionHeader("app/fileselect/group_other"_i18n, otherIndices.size(), otherBytes, !anySectionDrawn);
        anySectionDrawn = true;

        std::string basePrefix = computeBaseFolder(files_);
        if (!basePrefix.empty()) basePrefix += "/";

        OtherFileTreeNode rootNode;
        for (size_t i : otherIndices) {
            std::string normPath = files_[i].name;
            std::replace(normPath.begin(), normPath.end(), '\\', '/');

            std::string relPath = normPath;
            if (!basePrefix.empty() && relPath.rfind(basePrefix, 0) == 0) {
                relPath = relPath.substr(basePrefix.size());
            }

            std::vector<std::string> parts = splitPath(relPath);
            if (parts.empty()) continue;

            OtherFileTreeNode* curr = &rootNode;
            std::string currPath;
            for (size_t p = 0; p + 1 < parts.size(); ++p) {
                const std::string& seg = parts[p];
                if (!currPath.empty()) currPath += "/";
                currPath += seg;

                auto& sub = curr->subfolders[seg];
                if (sub.name.empty()) {
                    sub.name = seg;
                    sub.fullPath = currPath;
                    sub.isFolder = true;
                }
                sub.allFileIndices.push_back(i);
                sub.size += files_[i].size;
                sub.totalFiles++;
                if (i < selected_.size() && selected_[i]) {
                    sub.selectedFiles++;
                }
                curr = &sub;
            }

            OtherFileTreeNode leaf;
            leaf.name = parts.back();
            leaf.fullPath = normPath;
            leaf.isFolder = false;
            leaf.fileIndex = i;
            leaf.size = files_[i].size;
            curr->directFiles.push_back(leaf);
        }

        renderOtherFilesTree(rootNode, 0, focusMap);
    }

    // 3. Group: INSTALLED (only when non-empty; collapsible spoiler if uninstalled exist)
    if (!installedIndices.empty()) {
        const bool useSpoiler = !uninstalledIndices.empty();
        if (useSpoiler) {
            auto* spoiler = new brls::Box();
            spoiler->setAxis(brls::Axis::ROW);
            spoiler->setAlignItems(brls::AlignItems::CENTER);
            spoiler->setHeight(48.0f);
            spoiler->setWidth(brls::View::AUTO);
            spoiler->setMarginTop(anySectionDrawn ? 12.0f : 2.0f);
            spoiler->setMarginBottom(6.0f);
            spoiler->setPaddingLeft(14.0f);
            spoiler->setPaddingRight(16.0f);
            spoiler->setCornerRadius(10.0f);
            spoiler->setBackgroundColor(rowCardBg());
            spoiler->setBorderColor(rowCardBorder());
            spoiler->setBorderThickness(1.0f);
            spoiler->setFocusable(true);
            spoiler->getFocusEvent()->subscribe([this, spoiler](bool focused) {
                if (focused) {
                    lastFocusedListRow_ = spoiler;
                    bindRightPanelLeftRoute(spoiler);
                }
            });

            auto* arrow = new brls::Label();
            arrow->setFontSize(14.0f);
            arrow->setWidth(20.0f);
            arrow->setText(installedExpanded_ ? "\u25BE" : "\u25B8");
            arrow->setTextColor(style::textSecondary());
            spoiler->addView(arrow);

            auto* titleLbl = new brls::Label();
            titleLbl->setFontSize(14.0f);
            titleLbl->setLetterSpacing(0.8f);
            titleLbl->setSingleLine(true);
            titleLbl->setGrow(1.0f);
            titleLbl->setShrink(1.0f);
            titleLbl->setText("app/fileselect/group_installed"_i18n);
            titleLbl->setTextColor(style::textSecondary());
            titleLbl->setMarginRight(10.0f);
            spoiler->addView(titleLbl);

            auto* metaLbl = new brls::Label();
            metaLbl->setFontSize(14.0f);
            metaLbl->setSingleLine(true);
            metaLbl->setText(util::pluralize("app/plural/files", static_cast<long long>(installedIndices.size())) +
                             " \u00B7 " + util::formatFileSize(installedBytes));
            metaLbl->setTextColor(style::textSecondary());
            metaLbl->setMarginRight(12.0f);
            spoiler->addView(metaLbl);

            auto* actionLbl = new brls::Label();
            actionLbl->setFontSize(14.0f);
            actionLbl->setSingleLine(true);
            actionLbl->setText(installedExpanded_ ? "app/fileselect/hide_btn"_i18n : "app/fileselect/show_btn"_i18n);
            actionLbl->setTextColor(ThemeManager::instance().getTextAccentColor());
            spoiler->addView(actionLbl);

            focusMap["installed_spoiler"] = spoiler;
            spoiler->registerClickAction([this](brls::View*) {
                installedExpanded_ = !installedExpanded_;
                focusedKey_ = "installed_spoiler";
                rebuildSwitchFileList();
                return true;
            });

            fileListBox->addView(spoiler);
            anySectionDrawn = true;

            if (installedExpanded_) {
                for (size_t idx : installedIndices) {
                    addSwitchPackageRow(idx, true, "game_inst:");
                }
            }
        } else {
            addSectionHeader("app/fileselect/group_installed"_i18n, installedIndices.size(), installedBytes, !anySectionDrawn);
            anySectionDrawn = true;
            for (size_t idx : installedIndices) {
                addSwitchPackageRow(idx, true, "game_inst:");
            }
        }
    }

    updateCrossColumnRoutes();

    // Focus restoration (only if we are not preserving right-panel focus)
    if (!keepRightFocus && !focusedKey_.empty()) {
        if (focusMap.count(focusedKey_)) {
            brls::Application::giveFocus(focusMap[focusedKey_]);
        } else {
            std::string prefix = focusedKey_;
            while (true) {
                size_t slash = prefix.rfind('/');
                if (slash == std::string::npos) break;
                prefix = prefix.substr(0, slash);
                if (focusMap.count(prefix)) {
                    brls::Application::giveFocus(focusMap[prefix]);
                    break;
                }
            }
        }
        focusedKey_.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// calculateUncompressedSizes
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::calculateUncompressedSizes() {
    if (files_.empty()) return;

    uint64_t total_torrent_bytes = 0;
    for (const auto& f : files_) {
        total_torrent_bytes += f.size;
    }

    double nsz_ratio = util::parseNszCompressionRatio(game_.image_format, total_torrent_bytes);
    util::logLine("FileSelectView: calculated NSZ ratio " + std::to_string(nsz_ratio) +
                  " from image_format='" + game_.image_format + "'");

    probe_info_.clear();
    probe_info_.resize(files_.size());
    for (size_t i = 0; i < files_.size(); ++i) {
        if (!isSwitchGameFile(files_[i].name)) {
            probe_info_[i].uncompressed_size = files_[i].size;
            probe_info_[i].is_estimated = false;
        } else {
            std::string lower = files_[i].name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.size() >= 4 && (lower.rfind(".nsz") == lower.size() - 4 || lower.rfind(".xcz") == lower.size() - 4)) {
                probe_info_[i].uncompressed_size = static_cast<uint64_t>(files_[i].size * nsz_ratio);
                probe_info_[i].is_estimated = (std::abs(nsz_ratio - 1.0) > 0.001);
            } else {
                probe_info_[i].uncompressed_size = files_[i].size;
                probe_info_[i].is_estimated = false;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Right Panel: storage stats, chips, no-install toggle, summary & navigation
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::refreshStorageStats() {
    const bool folderMode = isFolderHierarchyMode();
    auto alive = alive_flag_;
    brls::async([this, alive, folderMode]() {
        int64_t sdFree = 0, sdTotal = 0;
        int64_t nandFree = 0, nandTotal = 0;
        int64_t usbFree = 0, usbTotal = 0;
        bool sdOk = util::getStorageStats(1, sdFree, sdTotal);
        bool nandOk = !folderMode && util::getStorageStats(0, nandFree, nandTotal);
        bool usbOk = !folderMode && util::getUsbStorageStats(usbFree, usbTotal);
        bool usbAvail = usbOk && (usbTotal > 0);

        brls::sync([this, alive, sdOk, sdFree, sdTotal, nandOk, nandFree, nandTotal, usbOk, usbFree, usbTotal, usbAvail]() {
            if (!alive->load()) return;
            sdOk_ = sdOk;
            sdFree_ = sdFree;
            sdTotal_ = sdTotal;
            nandOk_ = nandOk;
            nandFree_ = nandFree;
            nandTotal_ = nandTotal;
            usbOk_ = usbOk;
            usbFree_ = usbFree;
            usbTotal_ = usbTotal;
            usbAvailable_ = usbAvail;

            auto& cfg = config::ConfigManager::instance();
            if (cfg.getInstallLocation() == "usb" && !usbAvailable_) {
                cfg.setInstallLocation("auto");
                cfg.save();
            }

            updateStorageChipsUi();
            updateSummaryUi();
            updateCrossColumnRoutes();
        });
    });
}

void FileSelectView::updateStorageChipsUi() {
    const bool fixedTarget = isFolderHierarchyMode() || isPcGame(game_);
    if (fixedTarget) return;

    auto& cfg = config::ConfigManager::instance();
    std::string loc = cfg.getInstallLocation();
    if (loc != "auto" && loc != "sd" && loc != "nand" && loc != "usb") {
        loc = "auto";
    }
    if (loc == "usb" && !usbAvailable_) {
        loc = "auto";
    }

    if (chipUsb) {
        chipUsb->setVisibility(usbAvailable_ ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    const NVGcolor accent = ThemeManager::instance().getAccentColor();
    const NVGcolor inactiveBg = chipInactiveBg();
    const NVGcolor inactiveFg = noInstall_ ? style::textSecondary() : style::textPrimary();
    const NVGcolor activeBg = noInstall_ ? ThemeManager::instance().getDimAccentColor() : accent;
    const NVGcolor activeFg = noInstall_ ? style::textSecondary() : contrastTextOn(accent);

    auto styleChip = [&](brls::Box* chip, brls::Label* lbl, bool active) {
        if (!chip || !lbl) return;
        chip->setBackgroundColor(active ? activeBg : inactiveBg);
        chip->setBorderColor(active ? (noInstall_ ? style::cardBorder() : accent) : style::cardBorder());
        chip->setBorderThickness(1.0f);
        lbl->setTextColor(active ? activeFg : inactiveFg);
    };

    styleChip(chipAuto, chipAutoLabel, loc == "auto");
    styleChip(chipSd,   chipSdLabel,   loc == "sd");
    styleChip(chipNand, chipNandLabel, loc == "nand");
    if (usbAvailable_) {
        styleChip(chipUsb, chipUsbLabel, loc == "usb");
    }

    if (noInstallSwitch_) {
        noInstallSwitch_->setOn(noInstall_);
    }
    if (noInstallToggleRow) {
        noInstallToggleRow->setBackgroundColor(noInstall_ ? ThemeManager::instance().getDimAccentColor() : rowCardBg());
        noInstallToggleRow->setBorderColor(noInstall_ ? accent : rowCardBorder());
        noInstallToggleRow->setBorderThickness(1.0f);
    }
    if (noInstallLabel) {
        noInstallLabel->setTextColor(noInstall_ ? ThemeManager::instance().getTextAccentColor() : style::textPrimary());
    }
}

void FileSelectView::updateSummaryUi() {
    size_t selCount = 0;
    unsigned long long totalCompressed = 0;
    unsigned long long totalUnpacked = 0;
    bool anyEstimated = false;

    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            selCount++;
            totalCompressed += files_[i].size;
            if (!noInstall_) {
                if (i < probe_info_.size() && probe_info_[i].uncompressed_size > 0) {
                    totalUnpacked += probe_info_[i].uncompressed_size;
                    if (probe_info_[i].is_estimated) anyEstimated = true;
                } else {
                    totalUnpacked += files_[i].size;
                }
            }
        }
    }

    if (selectedCountText) {
        if (files_.empty()) {
            selectedCountText->setText("");
        } else {
            selectedCountText->setText(util::pluralize("app/plural/files", static_cast<long long>(selCount)));
        }
    }

    if (totalSizeText) {
        totalSizeText->setText(util::formatFileSize(totalCompressed));
    }

    if (unpackedSizeText) {
        if (!noInstall_ && totalUnpacked > totalCompressed) {
            std::string unpackedStr = util::formatFileSize(totalUnpacked);
            unpackedSizeText->setText(brls::getStr(
                anyEstimated ? "app/fileselect/unpacked_est" : "app/fileselect/unpacked_exact",
                unpackedStr));
            unpackedSizeText->setVisibility(brls::Visibility::VISIBLE);
        } else {
            unpackedSizeText->setVisibility(brls::Visibility::GONE);
        }
    }

    // Effective space requirement for target storage
    uint64_t neededBytes = (!noInstall_ && !isFolderHierarchyMode() && totalUnpacked > totalCompressed)
        ? totalUnpacked
        : totalCompressed;

    // Resolve active drive capacity & free space for bar + status
    const bool sdOnly = isFolderHierarchyMode() || isPcGame(game_) || noInstall_;
    auto& cfg = config::ConfigManager::instance();
    std::string loc = sdOnly ? "sd" : cfg.getInstallLocation();
    if (loc == "usb" && !usbAvailable_) loc = "auto";

    bool driveOk = false;
    int64_t driveFree = 0;
    int64_t driveTotal = 0;

    if (loc == "usb") {
        driveOk = usbOk_;
        driveFree = usbFree_;
        driveTotal = usbTotal_;
    } else if (loc == "nand") {
        driveOk = nandOk_;
        driveFree = nandFree_;
        driveTotal = nandTotal_;
    } else if (loc == "sd") {
        driveOk = sdOk_;
        driveFree = sdFree_;
        driveTotal = sdTotal_;
    } else { // auto: prefer SD if it fits, else NAND if it fits, else SD
        if (sdOk_ && static_cast<uint64_t>(std::max<int64_t>(0, sdFree_)) >= neededBytes) {
            driveOk = true;
            driveFree = sdFree_;
            driveTotal = sdTotal_;
        } else if (nandOk_ && static_cast<uint64_t>(std::max<int64_t>(0, nandFree_)) >= neededBytes) {
            driveOk = true;
            driveFree = nandFree_;
            driveTotal = nandTotal_;
        } else {
            driveOk = sdOk_;
            driveFree = sdFree_;
            driveTotal = sdTotal_;
        }
    }

    if (storageBar_) {
        storageBar_->setValues(driveOk ? driveTotal : 0, driveOk ? driveFree : 0, neededBytes);
    }

    if (spaceStatusText) {
        if (!driveOk) {
            spaceStatusText->setText(brls::getStr("app/fileselect/free_sd", "app/fileselect/free_unknown"_i18n));
            spaceStatusText->setTextColor(style::textSecondary());
        } else if (neededBytes > static_cast<uint64_t>(std::max<int64_t>(0, driveFree))) {
            uint64_t deficit = neededBytes - static_cast<uint64_t>(std::max<int64_t>(0, driveFree));
            spaceStatusText->setText(brls::getStr("app/fileselect/not_enough_space_short", util::formatFileSize(deficit)));
            spaceStatusText->setTextColor(style::stateError());
        } else {
            uint64_t remain = static_cast<uint64_t>(driveFree) - neededBytes;
            spaceStatusText->setText(brls::getStr("app/fileselect/free_after_short", util::formatFileSize(remain)));
            spaceStatusText->setTextColor(neededBytes > 0 ? style::stateOk() : style::textPrimary());
        }
    }

    if (secondarySpaceText) {
        std::string sdPart = "SD: " + (sdOk_ ? util::formatFileSize(static_cast<unsigned long long>(std::max<int64_t>(0, sdFree_))) : "---");
        if (isFolderHierarchyMode() || isPcGame(game_) || noInstall_) {
            secondarySpaceText->setText(sdPart);
        } else if (loc == "usb" && usbAvailable_) {
            std::string usbPart = "USB: " + (usbOk_ ? util::formatFileSize(static_cast<unsigned long long>(std::max<int64_t>(0, usbFree_))) : "---");
            secondarySpaceText->setText(usbPart + " \u00B7 " + sdPart);
        } else {
            std::string nandPart = "NAND: " + (nandOk_ ? util::formatFileSize(static_cast<unsigned long long>(std::max<int64_t>(0, nandFree_))) : "---");
            secondarySpaceText->setText(sdPart + " \u00B7 " + nandPart);
        }
    }

    if (btnDownload) {
        if (selCount > 0) {
            btnDownload->setText(brls::getStr("app/fileselect/download_btn_count", std::to_string(selCount)));
            btnDownload->setAlpha(1.0f);
        } else if (files_.empty() && leftState_ == LeftState::Error) {
            btnDownload->setText("app/fileselect/download_all_btn"_i18n);
            btnDownload->setAlpha(1.0f);
        } else {
            btnDownload->setText("app/fileselect/download_btn"_i18n);
            btnDownload->setAlpha(0.55f);
        }
    }

    bool anySelected = (selCount > 0);
    if (btnToggleAll) {
        btnToggleAll->setText(anySelected ? "app/fileselect/deselect_all"_i18n : "app/fileselect/select_all"_i18n);
        btnToggleAll->setAlpha(files_.empty() ? 0.55f : 1.0f);
    }
    if (auto* cv = this->getContentView()) {
        cv->updateActionHint(brls::ControllerButton::BUTTON_X,
                             anySelected ? "app/fileselect/deselect_all"_i18n : "app/fileselect/select_all"_i18n);
    }
}

void FileSelectView::updateTotalSize() {
    updateSummaryUi();
    refreshStorageStats();
}

void FileSelectView::selectStorageLocation(const std::string& loc, brls::View* clickedChip) {
    bool modeChanged = false;
    if (noInstall_) {
        noInstall_ = false;
        modeChanged = true;
    }
    auto& cfg = config::ConfigManager::instance();
    if (cfg.getInstallLocation() != loc) {
        cfg.setInstallLocation(loc);
        cfg.save();
    }
    updateStorageChipsUi();
    updateSummaryUi();
    if (modeChanged && !files_.empty() && !isFolderHierarchyMode()) {
        rebuildSwitchFileList(true /* keepRightFocus */);
        if (clickedChip) {
            brls::Application::giveFocus(clickedChip);
        }
    }
}

void FileSelectView::toggleNoInstallMode() {
    noInstall_ = !noInstall_;
    if (noInstall_ && util::isSdCardFat32()) {
        for (size_t i = 0; i < files_.size() && i < selected_.size(); ++i) {
            if (static_cast<uint64_t>(files_[i].size) > util::kFat32MaxFileSize) {
                selected_[i] = false;
            }
        }
    }
    updateStorageChipsUi();
    updateSummaryUi();
    if (!files_.empty() && !isFolderHierarchyMode()) {
        rebuildSwitchFileList(true /* keepRightFocus */);
        if (noInstallToggleRow) {
            brls::Application::giveFocus(noInstallToggleRow);
        }
    }
}

brls::View* FileSelectView::getRightPanelEntryView() {
    const bool fixedTarget = isFolderHierarchyMode() || isPcGame(game_);
    if (!fixedTarget && chipAuto) return chipAuto;
    return btnDownload;
}

brls::View* FileSelectView::getLeftPanelFallbackView() {
    if (leftState_ == LeftState::Loading) return loadingStateBox;
    if (leftState_ == LeftState::Error) return btnRetry ? static_cast<brls::View*>(btnRetry) : static_cast<brls::View*>(btnDownloadAll);
    if (fileListBox) {
        for (auto* child : fileListBox->getChildren()) {
            if (child && child->isFocusable()) {
                return child;
            }
        }
    }
    return nullptr;
}

void FileSelectView::bindRightPanelLeftRoute(brls::View* target) {
    if (chipAuto) chipAuto->setCustomNavigationRoute(brls::FocusDirection::LEFT, target);
    if (noInstallToggleRow) noInstallToggleRow->setCustomNavigationRoute(brls::FocusDirection::LEFT, target);
    if (btnDownload) btnDownload->setCustomNavigationRoute(brls::FocusDirection::LEFT, target);
    if (btnToggleAll) btnToggleAll->setCustomNavigationRoute(brls::FocusDirection::LEFT, target);
}

void FileSelectView::updateCrossColumnRoutes() {
    const bool fixedTarget = isFolderHierarchyMode() || isPcGame(game_);

    // Horizontal & vertical wiring inside right panel
    if (!fixedTarget) {
        std::vector<brls::Box*> visibleChips;
        if (chipAuto) visibleChips.push_back(chipAuto);
        if (chipSd)   visibleChips.push_back(chipSd);
        if (chipNand) visibleChips.push_back(chipNand);
        if (chipUsb && usbAvailable_) visibleChips.push_back(chipUsb);

        for (size_t i = 0; i < visibleChips.size(); ++i) {
            brls::Box* c = visibleChips[i];
            if (i > 0) {
                c->setCustomNavigationRoute(brls::FocusDirection::LEFT, visibleChips[i - 1]);
            }
            if (i + 1 < visibleChips.size()) {
                c->setCustomNavigationRoute(brls::FocusDirection::RIGHT, visibleChips[i + 1]);
            } else {
                c->setCustomNavigationRoute(brls::FocusDirection::RIGHT, nullptr);
            }
            if (noInstallToggleRow) {
                c->setCustomNavigationRoute(brls::FocusDirection::DOWN, noInstallToggleRow);
            }
        }
        if (noInstallToggleRow) {
            if (!visibleChips.empty()) {
                noInstallToggleRow->setCustomNavigationRoute(brls::FocusDirection::UP, visibleChips.front());
            }
            if (btnDownload) {
                noInstallToggleRow->setCustomNavigationRoute(brls::FocusDirection::DOWN, btnDownload);
            }
        }
        if (btnDownload) {
            btnDownload->setCustomNavigationRoute(brls::FocusDirection::UP,
                noInstallToggleRow ? static_cast<brls::View*>(noInstallToggleRow)
                                   : (!visibleChips.empty() ? static_cast<brls::View*>(visibleChips.front()) : nullptr));
            if (btnToggleAll) {
                btnDownload->setCustomNavigationRoute(brls::FocusDirection::DOWN, btnToggleAll);
            }
        }
        if (btnToggleAll && btnDownload) {
            btnToggleAll->setCustomNavigationRoute(brls::FocusDirection::UP, btnDownload);
        }
    } else {
        if (btnDownload && btnToggleAll) {
            btnDownload->setCustomNavigationRoute(brls::FocusDirection::DOWN, btnToggleAll);
            btnToggleAll->setCustomNavigationRoute(brls::FocusDirection::UP, btnDownload);
        }
    }

    brls::View* rightEntry = getRightPanelEntryView();
    if (loadingStateBox) {
        loadingStateBox->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rightEntry);
    }
    if (btnRetry && btnDownloadAll) {
        btnRetry->setCustomNavigationRoute(brls::FocusDirection::RIGHT, btnDownloadAll);
        btnDownloadAll->setCustomNavigationRoute(brls::FocusDirection::LEFT, btnRetry);
        btnDownloadAll->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rightEntry);
    }

    if (fileListBox) {
        for (auto* child : fileListBox->getChildren()) {
            if (child && child->isFocusable()) {
                child->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rightEntry);
            }
        }
    }

    brls::View* leftTarget = lastFocusedListRow_ ? lastFocusedListRow_ : getLeftPanelFallbackView();
    bindRightPanelLeftRoute(leftTarget);
}

// ─────────────────────────────────────────────────────────────────────────────
// toggleAllSelection & in-place checkbox updates
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::updateRowSelectionState(size_t idx) {
    if (idx >= fileRowBindings_.size()) return;
    const auto& binding = fileRowBindings_[idx];
    if (!binding.checkbox) return;

    if (binding.tooLarge) {
        binding.checkbox->setState(VectorCheckboxView::State::Disabled);
        return;
    }

    bool isSel = (idx < selected_.size()) && selected_[idx];
    binding.checkbox->setState(isSel ? VectorCheckboxView::State::Checked : VectorCheckboxView::State::Unchecked);
}

void FileSelectView::refreshFolderCheckboxes() {
    for (const auto& fb : folderRowBindings_) {
        if (!fb.checkbox) continue;
        size_t selCount = 0;
        for (size_t idx : fb.fileIndices) {
            if (idx < selected_.size() && selected_[idx]) selCount++;
        }
        bool allSel = (!fb.fileIndices.empty() && selCount == fb.fileIndices.size());
        bool partialSel = (selCount > 0 && !allSel);
        fb.checkbox->setState(allSel ? VectorCheckboxView::State::Checked
                                     : (partialSel ? VectorCheckboxView::State::Partial : VectorCheckboxView::State::Unchecked));
    }
}

void FileSelectView::toggleAllSelection() {
    if (selected_.empty()) return;

    if (isFolderHierarchyMode() && currentFolder_ != baseFolder_) {
        // Inside a subfolder: toggle selection for files inside currentFolder_
        std::string prefix = currentFolder_ + "/";
        std::vector<size_t> folderIndices;
        bool anySelectedInFolder = false;

        for (size_t i = 0; i < files_.size(); ++i) {
            std::string normPath = files_[i].name;
            std::replace(normPath.begin(), normPath.end(), '\\', '/');
            if (normPath.rfind(prefix, 0) == 0) {
                folderIndices.push_back(i);
                if (i < selected_.size() && selected_[i]) {
                    anySelectedInFolder = true;
                }
            }
        }

        bool newState = !anySelectedInFolder;
        for (size_t idx : folderIndices) {
            if (idx < selected_.size()) {
                selected_[idx] = newState;
                updateRowSelectionState(idx);
            }
        }
        refreshFolderCheckboxes();
    } else {
        bool anySelected = false;
        for (bool s : selected_) {
            if (s) { anySelected = true; break; }
        }
        const bool isFat = util::isSdCardFat32();
        for (size_t i = 0; i < selected_.size(); ++i) {
            if (!anySelected) {
                if (noInstall_ && isFat && static_cast<uint64_t>(files_[i].size) > util::kFat32MaxFileSize) {
                    selected_[i] = false;
                } else {
                    selected_[i] = true;
                }
            } else {
                selected_[i] = false;
            }
            updateRowSelectionState(i);
        }
        refreshFolderCheckboxes();
    }
    updateSummaryUi();
    updateHeaderMeta();
}

// ─────────────────────────────────────────────────────────────────────────────
// startDownloadAndGoToDownloads
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::startDownloadAndGoToDownloads() {
    if (files_.empty()) {
        ui::DownloadManager::instance().addDownload(game_, {}, -1, "", retro_console_id_, "", "", "", "", 0, 0, noInstall_);
        brls::Application::notify(brls::getStr("app/downloads/state_queued"));
        brls::Application::popActivity();
        return;
    }

    std::vector<int> selectedIndices;
    int forcedIndex = -1;
    std::string forcedName;
    unsigned long long largestGameSize = 0;
    unsigned long long totalNeededSize = 0;

    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            selectedIndices.push_back(files_[i].index);
            uint64_t needed = files_[i].size;
            if (!noInstall_ && i < probe_info_.size() && probe_info_[i].uncompressed_size > 0) {
                needed = probe_info_[i].uncompressed_size;
            }
            totalNeededSize += needed;
            if (isSwitchGameFile(files_[i].name) && files_[i].size > largestGameSize) {
                largestGameSize = files_[i].size;
                forcedIndex     = files_[i].index;
                forcedName      = files_[i].name;
            }
        }
    }

    if (selectedIndices.empty()) {
        brls::Application::notify("app/fileselect/no_files_selected"_i18n);
        return;
    }

    // Safety check for FAT32 in no_install mode
    if (noInstall_ && util::isSdCardFat32()) {
        for (size_t i = 0; i < files_.size(); ++i) {
            if (i < selected_.size() && selected_[i] && static_cast<uint64_t>(files_[i].size) > util::kFat32MaxFileSize) {
                std::string msg = brls::getStr("app/fileselect/fat32_cannot_download_dlg", files_[i].name, util::formatFileSize(files_[i].size));
                brls::Dialog* dialog = new brls::Dialog(msg);
                dialog->addButton("app/common/ok"_i18n, []() {});
                dialog->open();
                return;
            }
        }
    }

    // Fallback: largest file overall
    if (forcedIndex < 0) {
        unsigned long long maxOverall = 0;
        for (size_t i = 0; i < files_.size(); ++i) {
            if (i < selected_.size() && selected_[i] && files_[i].size > maxOverall) {
                maxOverall  = files_[i].size;
                forcedIndex = files_[i].index;
                forcedName  = files_[i].name;
            }
        }
    }

    if (isFolderHierarchyMode() || noInstall_) {
        int64_t freeSpace = 0;
        util::getStorageFreeSpace(1, freeSpace);
        if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            std::string msg = brls::getStr("app/fileselect/low_space_prompt", "SD", util::formatFileSize(totalNeededSize), util::formatFileSize(freeSpace));
            brls::Dialog* dialog = new brls::Dialog(msg);
            dialog->addButton("app/common/continue"_i18n, [this, selectedIndices, forcedIndex, forcedName]() {
                this->promptAndExecuteDownloads(selectedIndices, forcedIndex, forcedName);
            });
            dialog->addButton("app/common/cancel"_i18n, []() {});
            dialog->open();
            return;
        }
        promptAndExecuteDownloads(selectedIndices, forcedIndex, forcedName);
        return;
    }

    // Check free space depending on install_location setting
    auto& cfg = config::ConfigManager::instance();
    std::string loc = cfg.getInstallLocation();

    int64_t freeSpace = 0;
    std::string targetStorageName;
    bool checkPassed = true;

    if (loc == "usb") {
        targetStorageName = "app/fileselect/storage_usb"_i18n;
        if (!util::getUsbStorageFreeSpace(freeSpace)) {
            checkPassed = false;
        } else if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            checkPassed = false;
        }
    } else if (loc == "sd") {
        targetStorageName = "app/fileselect/storage_sd"_i18n;
        util::getStorageFreeSpace(1, freeSpace);
        if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            checkPassed = false;
        }
    } else if (loc == "nand") {
        targetStorageName = "app/fileselect/storage_nand"_i18n;
        util::getStorageFreeSpace(0, freeSpace);
        if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            checkPassed = false;
        }
    } else { // "auto"
        targetStorageName = "app/fileselect/storage_auto"_i18n;
        int64_t sdFree = 0;
        int64_t nandFree = 0;
        util::getStorageFreeSpace(1, sdFree);
        util::getStorageFreeSpace(0, nandFree);

        if (static_cast<int64_t>(totalNeededSize) <= sdFree) {
            freeSpace = sdFree;
        } else if (static_cast<int64_t>(totalNeededSize) <= nandFree) {
            freeSpace = nandFree;
        } else {
            freeSpace = sdFree; // default display
            checkPassed = false;
        }
    }

    if (!checkPassed) {
        std::string msg = brls::getStr("app/fileselect/low_space_prompt", targetStorageName, util::formatFileSize(totalNeededSize), util::formatFileSize(freeSpace));

        brls::Dialog* dialog = new brls::Dialog(msg);
        // Dialog closes itself via Dialog::buttonClick; do NOT call close() here
        // (it would pop this FileSelectView too).
        dialog->addButton("app/common/continue"_i18n, [this, selectedIndices, forcedIndex, forcedName]() {
            this->executeDownloads(selectedIndices, forcedIndex, forcedName);
        });
        dialog->addButton("app/common/cancel"_i18n, []() {});
        dialog->open();
        return;
    }

    executeDownloads(selectedIndices, forcedIndex, forcedName);
}

void FileSelectView::promptAndExecuteDownloads(const std::vector<int>& selectedIndices, int forcedIndex, const std::string& forcedName) {
    executeDownloads(selectedIndices, forcedIndex, forcedName);
}

void FileSelectView::executeDownloads(const std::vector<int>& selectedIndices, int forcedIndex, const std::string& forcedName) {
    (void)selectedIndices;
    (void)forcedIndex;
    (void)forcedName;

    std::vector<size_t> chosen;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            chosen.push_back(i);
        }
    }

    try {
        if (isFolderHierarchyMode()) {
            if (chosen.size() == 1) {
                size_t i = chosen[0];
                std::vector<int> singleSelected = { files_[i].index };
                std::string filenameOnly = std::filesystem::path(files_[i].name).filename().string();
                std::string itemTitle = cleanTitle(game_.title) + " (" + filenameOnly + ")";
                Game singleGame = game_;
                singleGame.title = itemTitle;
                singleGame.topic_id = game_.topic_id + "_" + std::to_string(files_[i].index);
                unsigned long long fileSize = (files_[i].size > 0) ? static_cast<unsigned long long>(files_[i].size) : 0;
                ui::DownloadManager::instance().addDownload(
                    singleGame, singleSelected, files_[i].index, files_[i].name, retro_console_id_,
                    game_.topic_id, cleanTitle(game_.title), "OTH", filenameOnly, 4, fileSize);
            } else if (!chosen.empty()) {
                std::vector<int> bundleSelected;
                bundleSelected.reserve(chosen.size());
                unsigned long long bundleTotalBytes = 0;
                for (size_t i : chosen) {
                    bundleSelected.push_back(files_[i].index);
                    if (files_[i].size > 0) bundleTotalBytes += static_cast<unsigned long long>(files_[i].size);
                }
                std::string bundleTitle = cleanTitle(game_.title) + " (" + util::pluralize("app/plural/files", static_cast<long long>(chosen.size())) + ")";
                Game bundleGame = game_;
                bundleGame.title = bundleTitle;
                bundleGame.topic_id = game_.topic_id + (retro_console_id_.empty() ? "_hb_bundle" : "_romset");
                int firstIndex = files_[chosen[0]].index;
                std::string firstName = files_[chosen[0]].name;
                ui::DownloadManager::instance().addDownload(
                    bundleGame, bundleSelected, firstIndex, firstName, retro_console_id_,
                    game_.topic_id, cleanTitle(game_.title), "OTH", util::pluralize("app/plural/files", static_cast<long long>(chosen.size())), 4, bundleTotalBytes);
            }
        } else {
            std::vector<size_t> packages;
            std::vector<size_t> extraFiles;

            for (size_t idx : chosen) {
                if (isSwitchGameFile(files_[idx].name)) {
                    packages.push_back(idx);
                } else {
                    extraFiles.push_back(idx);
                }
            }

            // Sort selected packages by install priority: Base (v0) -> Updates -> DLC
            std::stable_sort(packages.begin(), packages.end(), [this](size_t a, size_t b) {
                int prio_a = download::installFilePriority(files_[a].name);
                int prio_b = download::installFilePriority(files_[b].name);
                if (prio_a != prio_b) {
                    return prio_a > prio_b;
                }
                if (prio_a >= 150 && prio_a < 200) {
                    return files_[a].size > files_[b].size;
                }
                return files_[a].index < files_[b].index;
            });

            // 1. Queue installable Switch packages individually
            for (size_t idx : packages) {
                queueSwitchPackage(game_, files_[idx], noInstall_);
            }

            // 2. Bundle extra files (translations, patches, mods) into 1 download
            if (!extraFiles.empty()) {
                std::vector<int> extraIndices;
                extraIndices.reserve(extraFiles.size());
                unsigned long long extraTotalBytes = 0;
                for (size_t idx : extraFiles) {
                    extraIndices.push_back(files_[idx].index);
                    if (files_[idx].size > 0) extraTotalBytes += static_cast<unsigned long long>(files_[idx].size);
                }

                auto comp = util::classifyDownloadComponent(files_[extraFiles[0]].name, game_.title);

                std::string extraTitle = cleanTitle(game_.title);
                Game extraGame = game_;
                extraGame.title = extraTitle;
                extraGame.topic_id = game_.topic_id + "_extras";
                int firstIndex = files_[extraFiles[0]].index;
                std::string extraForcedName = files_[extraFiles[0]].name;

                ui::DownloadManager::instance().addDownload(
                    extraGame, extraIndices, firstIndex, extraForcedName, retro_console_id_,
                    game_.topic_id, cleanTitle(game_.title), comp.label, comp.cleanName, comp.type, extraTotalBytes, noInstall_);
            }
        }
    } catch (const std::exception& e) {
        brls::Logger::error("FileSelectView: failed to queue downloads: {}", e.what());
        brls::Application::notify(brls::getStr("app/fileselect/queue_add_failed", std::string(e.what())));
    }

    brls::sync([]() {
        brls::Application::popActivity(brls::TransitionAnimation::NONE, []() {
            brls::Application::pushActivity(new ui::DownloadsView());
        });
    });
}

} // namespace ui
