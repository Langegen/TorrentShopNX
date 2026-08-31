#include "LibraryView.hpp"
#include "CatalogView.hpp"
#include "GameDetailView.hpp"
#include "../utils/log.h"
#include "../utils/switch_utils.h"
#include "../utils/app_paths.h"
#include "../utils/mod_utils.h"
#include "../catalog/IgnoredUpdatesManager.hpp"
#include <exception>
#include <memory>
#include <fstream>
#include <unordered_map>
#include <sys/stat.h>
#include <ctime>
#include <algorithm>
#include "../net/http_client.h"

#ifdef __SWITCH__
#include <switch.h>
#include <mutex>

std::recursive_mutex g_switch_service_mutex;

// Returns the raw installed patch version (Nintendo units) for each base title
// id, using a single ncm service session instead of one init/exit per game.
static std::unordered_map<uint64_t, uint32_t> getInstalledPatchVersions(const std::vector<uint64_t>& baseTids) {
    std::unordered_map<uint64_t, uint32_t> result;
    if (baseTids.empty()) return result;

    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return result;

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_BuiltInUser);
    if (R_SUCCEEDED(rc)) {
        for (uint64_t baseTid : baseTids) {
            uint64_t patchTid = baseTid | 0x800ULL;
            NcmContentMetaKey key;
            if (R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, patchTid))) {
                result[baseTid] = key.version;
            }
        }
        ncmContentMetaDatabaseClose(&db);
    }
    ncmExit();
    return result;
}

static std::string iconCachePathFor(uint64_t tid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
    return std::string(TSNX_CACHE_ICONS) + "/" + buf + ".jpg";
}

// Extracts the system icon (JPEG embedded in the NACP control data, the same
// icon the Switch home menu shows) and caches it to the SD card.
static void saveInstalledIcon(uint64_t tid, const NsApplicationControlData& ctrl, size_t ctrlSize) {
    std::string path = iconCachePathFor(tid);
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) return; // already cached

    size_t nacpSize = sizeof(NacpStruct);
    if (ctrlSize <= nacpSize + 4) return;

    const u8* icon = ctrl.icon;
    size_t iconAvail = sizeof(ctrl.icon);
    if (iconAvail > ctrlSize - nacpSize) iconAvail = ctrlSize - nacpSize;

    // Locate the JPEG SOI marker (FF D8)
    size_t start = 0;
    bool foundStart = false;
    for (size_t i = 0; i + 1 < iconAvail; ++i) {
        if (icon[i] == 0xFF && icon[i + 1] == 0xD8) { start = i; foundStart = true; break; }
    }
    if (!foundStart) return;

    // Locate the JPEG EOI marker (FF D9)
    size_t end = start;
    for (size_t i = start; i + 1 < iconAvail; ++i) {
        if (icon[i] == 0xFF && icon[i + 1] == 0xD9) { end = i + 2; }
    }
    if (end <= start + 4) return;

    std::error_code ec;
    std::filesystem::create_directories(TSNX_CACHE_ICONS, ec);
    std::ofstream out(path, std::ios::binary);
    if (out) {
        out.write(reinterpret_cast<const char*>(&icon[start]), static_cast<std::streamsize>(end - start));
        util::logLine("library: cached system icon for " + path);
    }
}
#endif

extern std::vector<Game> g_games;

namespace ui {

static std::string formatBytes(unsigned long long bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return std::string(buf);
}

LibraryRowCell::LibraryRowCell() {
    this->inflateFromXMLRes("xml/library_row_cell.xml");
}

LibraryRowCell::~LibraryRowCell() {
    if (imageToken) *imageToken = false;
}

LibraryRowCell* LibraryRowCell::create() {
    return new LibraryRowCell();
}

static void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

static std::string cleanNameForMatching(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    
    // Replace common variants
    replaceAll(lower, "&", "and");
    replaceAll(lower, "part 1", "1");
    replaceAll(lower, "part i", "1");
    replaceAll(lower, "part 2", "2");
    replaceAll(lower, "part ii", "2");
    replaceAll(lower, "part 3", "3");
    replaceAll(lower, "part iii", "3");
    replaceAll(lower, "part 4", "4");
    replaceAll(lower, "part iv", "4");
    replaceAll(lower, "part 5", "5");
    replaceAll(lower, "part v", "5");
    
    // Replace roman numerals at word boundaries or ends of string
    replaceAll(lower, " iii", " 3");
    replaceAll(lower, " ii", " 2");
    replaceAll(lower, " iv", " 4");
    replaceAll(lower, " ix", " 9");
    replaceAll(lower, " viii", " 8");
    replaceAll(lower, " vii", " 7");
    replaceAll(lower, " vi", " 6");
    replaceAll(lower, " v", " 5");
    replaceAll(lower, " x", " 10");
    
    std::string result;
    bool inBrackets = false;
    int parenDepth = 0;
    
    for (size_t i = 0; i < lower.size(); ++i) {
        unsigned char c = lower[i];
        
        // Handle common UTF-8 chars (2-byte sequences starting with 0xC3)
        if (c == 0xC3 && i + 1 < lower.size()) {
            unsigned char next = lower[i+1];
            char replacement = 0;
            if (next >= 0xA8 && next <= 0xAB) replacement = 'e';
            else if (next >= 0xA0 && next <= 0xA4) replacement = 'a';
            else if (next >= 0xB2 && next <= 0xB6) replacement = 'o';
            else if (next >= 0xB9 && next <= 0xBC) replacement = 'u';
            else if (next >= 0xAC && next <= 0xAF) replacement = 'i';
            else if (next == 0xB1) replacement = 'n';
            
            if (replacement != 0 && !inBrackets && parenDepth == 0) {
                result.push_back(replacement);
            }
            i++; // skip next byte
            continue;
        }
        
        if (c == '[' || c == '{') {
            inBrackets = true;
        } else if (c == ']' || c == '}') {
            inBrackets = false;
        } else if (c == '(') {
            parenDepth++;
        } else if (c == ')') {
            if (parenDepth > 0) parenDepth--;
        } else if (!inBrackets && parenDepth == 0) {
            if (std::isalnum(c)) {
                result.push_back(static_cast<char>(c));
            }
        }
    }
    
    if (result.empty()) {
        for (char c : lower) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                result.push_back(c);
            }
        }
    }
    return result;
}

static std::string getFirstTwoWords(const std::string& str) {
    std::stringstream ss(str);
    std::string word1, word2;
    if (ss >> word1) {
        if (ss >> word2) {
            return word1 + " " + word2;
        }
        return word1;
    }
    return "";
}

static bool downloadVersionsDatabaseIfNeeded() {
    std::string path = TSNX_VERSIONS_PATH;
    struct stat st;
    bool exists = (stat(path.c_str(), &st) == 0);
    
    // If it exists and is less than 24 hours old, don't download
    if (exists) {
        time_t now = time(nullptr);
        if (now - st.st_mtime < 24 * 3600) {
            util::logLine("versions: database is up-to-date");
            return true;
        }
    }
    
    util::logLine("versions: downloading versions.txt from titledb (jsdelivr)");
    net::HttpClient http;
    auto res = http.httpGet("https://cdn.jsdelivr.net/gh/blawar/titledb@master/versions.txt");
    if (res.status_code != 200 || res.body.empty()) {
        util::logLine("versions: jsdelivr failed, trying raw github");
        res = http.httpGet("https://raw.githubusercontent.com/blawar/titledb/master/versions.txt");
    }
    
    if (res.status_code == 200 && !res.body.empty()) {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            out.write(res.body.data(), res.body.size());
            util::logLine("versions: database updated successfully");
            return true;
        }
    }
    
    util::logLine("versions: failed to download database, status=" + std::to_string(res.status_code));
    return exists; // return true if we can fall back to old file
}

static std::unordered_map<uint64_t, uint32_t> parseVersionsDatabase() {
    std::string path = TSNX_VERSIONS_PATH;

    // Reuse the parsed database in memory unless the file changed on disk
    // (versions.txt updates at most daily; reparsing 3+ MB on every library
    // open is wasted work).
    static std::unordered_map<uint64_t, uint32_t> cached;
    static time_t cachedMtime = 0;
    struct stat st;
    time_t mtime = 0;
    if (stat(path.c_str(), &st) == 0) mtime = st.st_mtime;
    if (!cached.empty() && mtime == cachedMtime) {
        util::logLine("versions: using in-memory cache (" + std::to_string(cached.size()) + " entries)");
        return cached;
    }

    std::unordered_map<uint64_t, uint32_t> database;
    std::ifstream in(path);
    if (!in) {
        util::logLine("versions: cannot open versions.txt");
        return database;
    }
    
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        
        size_t firstPipe = line.find('|');
        if (firstPipe == std::string::npos) continue;
        
        size_t lastPipe = line.rfind('|');
        if (lastPipe == std::string::npos) continue;
        
        std::string tidStr = line.substr(0, firstPipe);
        std::string verStr = line.substr(lastPipe + 1);
        
        while (!verStr.empty() && (verStr.back() == '\r' || std::isspace(static_cast<unsigned char>(verStr.back())))) {
            verStr.pop_back();
        }
        
        try {
            uint64_t tid = std::stoull(tidStr, nullptr, 16);
            uint32_t ver = static_cast<uint32_t>(std::stoul(verStr));
            database[tid] = ver;
        } catch (...) {}
    }
    util::logLine("versions: parsed " + std::to_string(database.size()) + " entries");

    cached = database;
    cachedMtime = mtime;
    return database;
}

LibraryView::LibraryView() {
    cancelToken_ = std::make_shared<bool>(false);
}

LibraryView::~LibraryView() {
    if (cancelToken_) {
        *cancelToken_ = true;
    }
}

void LibraryView::onContentAvailable() {
    if (recycler) {
        recycler->registerCell("Row", []() { return LibraryRowCell::create(); });
        recycler->setDataSource(new LibraryDataSource(this));
    }

    updateSpaceHint();
    scanForUpdates();
}

void LibraryView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState && recycler && !sections_.empty()) {
        brls::Application::giveFocus(recycler);
    }
}

void LibraryView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

void LibraryView::rebuildSections() {
    sections_.clear();

    std::vector<LibraryItem> modsUpdates;
    std::vector<LibraryItem> normalUpdates;
    std::vector<LibraryItem> ignoredItems;
    std::vector<LibraryItem> upToDateItems;
    std::vector<LibraryItem> otherItems;

    auto& ignoredMgr = catalog::IgnoredUpdatesManager::instance();

    for (auto item : rawItems_) {
        item.updateIgnored = ignoredMgr.isIgnored(item.titleId);
        if (item.updateIgnored) {
            item.status = GameUpdateStatus::UpdateIgnored;
            ignoredItems.push_back(item);
        } else if (item.status == GameUpdateStatus::UpdateAvailable) {
            if (item.hasMods) {
                modsUpdates.push_back(item);
            } else {
                normalUpdates.push_back(item);
            }
        } else if (item.status == GameUpdateStatus::UpToDate) {
            upToDateItems.push_back(item);
        } else {
            otherItems.push_back(item);
        }
    }

    auto sortAlpha = [](std::vector<LibraryItem>& list) {
        std::sort(list.begin(), list.end(), [](const LibraryItem& a, const LibraryItem& b) {
            return cleanTitle(a.game.title) < cleanTitle(b.game.title);
        });
    };

    sortAlpha(modsUpdates);
    sortAlpha(normalUpdates);
    sortAlpha(ignoredItems);
    sortAlpha(upToDateItems);
    sortAlpha(otherItems);

    if (!modsUpdates.empty()) {
        std::string title = brls::getStr("app/library/section_mods_updates", std::to_string(modsUpdates.size()));
        sections_.push_back({title, std::move(modsUpdates)});
    }
    if (!normalUpdates.empty()) {
        std::string title = brls::getStr("app/library/section_updates", std::to_string(normalUpdates.size()));
        sections_.push_back({title, std::move(normalUpdates)});
    }
    if (!ignoredItems.empty()) {
        std::string title = brls::getStr("app/library/section_ignored", std::to_string(ignoredItems.size()));
        sections_.push_back({title, std::move(ignoredItems)});
    }
    if (!upToDateItems.empty()) {
        std::string title = brls::getStr("app/library/section_uptodate", std::to_string(upToDateItems.size()));
        sections_.push_back({title, std::move(upToDateItems)});
    }
    if (!otherItems.empty()) {
        std::string title = brls::getStr("app/library/section_other", std::to_string(otherItems.size()));
        sections_.push_back({title, std::move(otherItems)});
    }

    if (recycler) {
        recycler->reloadData();
    }
    updateStatsAndSpace();
}

void LibraryView::toggleUpdateIgnored(uint64_t titleId, const std::string& displayName) {
    bool nowIgnored = catalog::IgnoredUpdatesManager::instance().toggleIgnored(titleId);
    rebuildSections();
    if (recycler && !sections_.empty()) {
        brls::Application::giveFocus(recycler);
    }
    std::string name = displayName.empty() ? "?" : displayName;
    if (nowIgnored) {
        brls::Application::notify(brls::getStr("app/library/notify_update_disabled", name));
    } else {
        brls::Application::notify(brls::getStr("app/library/notify_update_enabled", name));
    }
}

void LibraryView::showModWarningDialog(const LibraryItem& item) {
    std::string displayName = item.rawName.empty() ? cleanTitle(item.game.title) : item.rawName;
    std::string modStr = item.modDetails.empty() ? "RomFS" : item.modDetails;
    std::string msg = brls::getStr("app/library/mod_warn_dialog_msg", displayName, modStr);

    brls::Dialog* dialog = new brls::Dialog(msg);
    Game game = item.game;
    std::string rawName = item.rawName;
    uint64_t tid = item.titleId;

    dialog->addButton("app/library/mod_warn_btn_proceed"_i18n, [game, rawName]() {
        if (!game.magnet.empty()) {
            brls::Application::pushActivity(new GameDetailView(game));
        } else {
            std::string query = getFirstTwoWords(rawName);
            if (query.empty()) query = rawName;
            brls::Application::pushActivity(new CatalogView(query));
        }
    });

    dialog->addButton("app/library/mod_warn_btn_freeze"_i18n, [this, tid, displayName]() {
        toggleUpdateIgnored(tid, displayName);
    });

    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

void LibraryView::updateStatsAndSpace() {
    int updateCount = 0;
    int modsCount = 0;
    int ignoredCount = 0;
    int totalCount = 0;

    for (const auto& item : rawItems_) {
        totalCount++;
        if (item.hasMods) modsCount++;
        if (item.updateIgnored) {
            ignoredCount++;
        } else if (item.status == GameUpdateStatus::UpdateAvailable) {
            updateCount++;
        }
    }

    if (statsHint) {
        std::string s = brls::getStr("app/library/stats", std::to_string(updateCount), std::to_string(totalCount));
        if (modsCount > 0) {
            s += " • " + brls::getStr("app/library/stats_mods", std::to_string(modsCount));
        }
        if (ignoredCount > 0) {
            s += " • " + brls::getStr("app/library/stats_ignored", std::to_string(ignoredCount));
        }
        statsHint->setText(s);
    }
    updateSpaceHint();
}

void LibraryView::updateSpaceHint() {
    if (!spaceHint) return;

    int64_t sdFree = 0;
    int64_t nandFree = 0;
    bool sdOk = util::getStorageFreeSpace(1, sdFree);
    bool nandOk = util::getStorageFreeSpace(0, nandFree);

    std::string sdStr = sdOk ? formatBytes(static_cast<unsigned long long>(sdFree)) : "app/library/space_unknown"_i18n;
    std::string nandStr = nandOk ? formatBytes(static_cast<unsigned long long>(nandFree)) : "app/library/space_unknown"_i18n;

    spaceHint->setText(brls::getStr("app/library/space", sdStr, nandStr));
}

void LibraryView::scanForUpdates() {
    if (isScanning_) {
        util::logLine("library: scan already in progress");
        return;
    }
    isScanning_ = true;
    
    if (statsHint) {
        statsHint->setText("app/library/loading_titledb"_i18n);
    }
    
    auto cancelToken = cancelToken_;
    brls::async([this, cancelToken]() {
        if (cancelToken && *cancelToken) return;

        bool dbOk = downloadVersionsDatabaseIfNeeded();
        if (cancelToken && *cancelToken) return;

        if (!dbOk) {
            brls::sync([this, cancelToken]() {
                if (cancelToken && *cancelToken) return;
                if (statsHint) {
                    statsHint->setText("app/library/titledb_failed"_i18n);
                }
                isScanning_ = false;
            });
            return;
        }
        
        brls::sync([this, cancelToken]() {
            if (cancelToken && *cancelToken) return;
            if (statsHint) {
                statsHint->setText("app/library/scanning_games"_i18n);
            }
        });
        
        auto availableVersions = parseVersionsDatabase();
        if (cancelToken && *cancelToken) return;
        
        struct InstalledItem {
            uint64_t titleId;
            std::string name;
            std::string currentVersionStr;
        };
        std::vector<InstalledItem> installedList;
        std::vector<uint64_t> baseTids;

#ifdef __SWITCH__
        std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
        Result rc = nsInitialize();
        if (R_SUCCEEDED(rc)) {
            s32 offset = 0;
            s32 entry_count = 0;
            std::vector<NsApplicationRecord> records;
            
            do {
                std::vector<NsApplicationRecord> batch(50);
                rc = nsListApplicationRecord(batch.data(), 50, offset, &entry_count);
                if (R_FAILED(rc)) break;
                
                for (s32 i = 0; i < entry_count; ++i) {
                    records.push_back(batch[i]);
                }
                offset += entry_count;
            } while (entry_count == 50);
            
            for (const auto& rec : records) {
                if (cancelToken && *cancelToken) break;

                uint64_t tid = rec.application_id;
                auto ctrl = std::make_unique<NsApplicationControlData>();
                size_t ctrl_size = 0;
                rc = nsGetApplicationControlData(
                    NsApplicationControlSource_Storage,
                    tid,
                    ctrl.get(),
                    sizeof(NsApplicationControlData),
                    &ctrl_size);
                
                std::string curVer = "1.0.0";
                std::string name = "";
                if (R_SUCCEEDED(rc) && ctrl_size >= sizeof(ctrl->nacp)) {
                    curVer = ctrl->nacp.display_version;
                    
                    // Try to get English name first (index 1 in NACP language array)
                    if (ctrl->nacp.lang[1].name[0] != '\0') {
                        name = ctrl->nacp.lang[1].name;
                    }
                    
                    // Fallback to system language if English is not available
                    if (name.empty()) {
                        NacpLanguageEntry* lang = nullptr;
                        if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &lang)) && lang) {
                            name = lang->name;
                        }
                    }

                    // Cache the system icon (the one the home menu shows)
                    saveInstalledIcon(tid, *ctrl, ctrl_size);
                }
                installedList.push_back({tid, name, curVer});
                baseTids.push_back(tid);
            }
            nsExit();
        }

        // Query installed patch versions in one ncm session (instead of one
        // init/exit per game — the old code was the main library slow-down).
        std::unordered_map<uint64_t, uint32_t> installedPatchVersions = getInstalledPatchVersions(baseTids);
#else
        // Mock data for PC testing
        installedList.push_back({0x0100000000010000ULL, "Super Mario Odyssey", "1.0.0"});
        installedList.push_back({0x01007ef00011e000ULL, "The Legend of Zelda: Breath of the Wild", "1.0.0"});
        installedList.push_back({0x0100000000020000ULL, "Unmatched Dummy Game", "1.1.0"});
        std::unordered_map<uint64_t, uint32_t> installedPatchVersions;
#endif

        if (cancelToken && *cancelToken) return;

        // Build a catalog index once per scan: exact title id lookup + a
        // pre-normalized name list, so per-game matching is O(1) instead of
        // re-running the expensive normalization for every catalog entry.
        struct CatalogIndex {
            std::unordered_map<uint64_t, const Game*> byTid;
            std::vector<std::pair<std::string, const Game*>> byCleanName;
        };
        CatalogIndex catIndex;
        catIndex.byTid.reserve(g_games.size());
        catIndex.byCleanName.reserve(g_games.size());
        for (const auto& g : g_games) {
            uint64_t tid = parseTitleIdFromGame(g);
            if (tid != 0) {
                if (catIndex.byTid.find(tid) == catIndex.byTid.end()) catIndex.byTid.emplace(tid, &g);
                uint64_t patchTid = tid | 0x800ULL;
                if (patchTid != tid && catIndex.byTid.find(patchTid) == catIndex.byTid.end()) {
                    catIndex.byTid.emplace(patchTid, &g);
                }
            }
            std::string cn = cleanNameForMatching(g.title);
            if (!cn.empty()) catIndex.byCleanName.emplace_back(std::move(cn), &g);
        }

        auto makeItem = [](const Game& g, const std::string& currentVer,
                           const std::string& latestVer, GameUpdateStatus status,
                           uint64_t baseTid, const std::string& rawName,
                           bool hasMods, bool updateIgnored, const std::string& modDetails) {
            LibraryItem item;
            item.game = g;
            item.currentVersion = currentVer;
            item.latestVersion = latestVer;
            item.status = status;
            item.titleId = baseTid;
            item.rawName = rawName;
            item.hasMods = hasMods;
            item.updateIgnored = updateIgnored;
            item.modDetails = modDetails;
            return item;
        };

        std::vector<LibraryItem> displayItems;

        // Match installed items with the catalog (g_games) using titledb
        for (const auto& inst : installedList) {
            if (cancelToken && *cancelToken) return;

            uint64_t baseTid = inst.titleId;
            uint64_t patchTid = baseTid | 0x800ULL; // Patch ID
            
            uint32_t latestVer = 0;
            auto it = availableVersions.find(patchTid);
            if (it != availableVersions.end()) {
                latestVer = it->second;
            }
            
            uint32_t currentVer = 0;
            auto iv = installedPatchVersions.find(baseTid);
            if (iv != installedPatchVersions.end()) {
                currentVer = iv->second;
            }
            
            // The raw titledb value (e.g. 196608) is a Nintendo "update number"
            // that cannot be reliably converted to real semver (1.2.0 style),
            // so display it honestly as v<number> — same convention tinfoil uses.
            bool hasVersionInfo = (latestVer > 0);
            GameUpdateStatus status = GameUpdateStatus::Unknown;
            std::string latestVerStr = "—";
            std::string currentVerStr = inst.currentVersionStr;
            if (currentVer > 0) {
                currentVerStr = "v" + std::to_string(currentVer);
            }
            
            if (hasVersionInfo) {
                latestVerStr = "v" + std::to_string(latestVer);
                bool needsUpdate = false;
                if (currentVer > 0) {
                    needsUpdate = (latestVer > currentVer);
                } else {
                    needsUpdate = true; // an update exists and none is installed
                }
                status = needsUpdate ? GameUpdateStatus::UpdateAvailable : GameUpdateStatus::UpToDate;
            }

            // Check if game has mods and if update check is ignored
            auto modInfo = util::detectGameMods(baseTid);
            bool isIgnored = catalog::IgnoredUpdatesManager::instance().isIgnored(baseTid);
            if (isIgnored) {
                status = GameUpdateStatus::UpdateIgnored;
            }
            
            // Match by Title ID (indexed): the explicit title_id field from the
            // catalog JSON, or the [0100...] bracket in the title.
            bool foundInCatalog = false;
            {
                auto mit = catIndex.byTid.find(baseTid);
                if (mit == catIndex.byTid.end()) mit = catIndex.byTid.find(patchTid);
                if (mit != catIndex.byTid.end()) {
                    displayItems.push_back(makeItem(*mit->second, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary));
                    foundInCatalog = true;
                }
            }
            
            // Fallback to name matching if Title ID match failed
            if (!foundInCatalog) {
                std::string instClean = cleanNameForMatching(inst.name);
                if (!instClean.empty()) {
                    for (const auto& entry : catIndex.byCleanName) {
                        const std::string& catClean = entry.first;
                        if (catClean == instClean || catClean.find(instClean) != std::string::npos || instClean.find(catClean) != std::string::npos) {
                            displayItems.push_back(makeItem(*entry.second, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary));
                            foundInCatalog = true;
                            break;
                        }
                    }
                }
            }
            
            // If not found in catalog, create a dummy item
            if (!foundInCatalog) {
                Game dummy;
                dummy.title = inst.name.empty() ? ("Unknown Game") : inst.name;
                dummy.cover = "";
                dummy.size = "";
                displayItems.push_back(makeItem(dummy, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary));
            }
        }
        
        if (cancelToken && *cancelToken) return;

        // Sync with UI thread
        brls::sync([this, cancelToken, items = std::move(displayItems)]() mutable {
            if (cancelToken && *cancelToken) return;

            isScanning_ = false;
            if (!this->getContentView()) return;
            
            rawItems_ = std::move(items);
            rebuildSections();
            
            if (!sections_.empty() && recycler) {
                brls::Application::giveFocus(recycler);
            }
        });
    });
}

void LibraryView::uninstallGame(uint64_t titleId, const std::string& displayName) {
    std::string msg = brls::getStr("app/library/uninstall_confirm", displayName.empty() ? "?" : displayName);
    
    brls::Dialog* dialog = new brls::Dialog(msg);
    dialog->addButton("app/common/yes"_i18n, [this, titleId, displayName]() {
        auto cancelToken = cancelToken_;
        brls::async([this, titleId, displayName, cancelToken]() {
            try {
                if (cancelToken && *cancelToken) return;

                bool ok = false;
#ifdef __SWITCH__
                {
                    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
                    Result rc = nsInitialize();
                    if (R_SUCCEEDED(rc)) {
                        rc = nsDeleteApplicationCompletely(titleId);
                        nsExit();
                        ok = R_SUCCEEDED(rc);
                        util::logLine("library: nsDeleteApplicationCompletely tid=" + std::to_string(titleId) + " rc=" + std::to_string(rc));
                    } else {
                        util::logLine("library: nsInitialize failed rc=" + std::to_string(rc));
                    }
                }
#else
                ok = true; // PC mock: allow uninstall
#endif
                brls::sync([this, titleId, displayName, ok, cancelToken]() {
                    if (cancelToken && *cancelToken) return;

                    if (ok) {
                        try {
                            for (auto it = rawItems_.begin(); it != rawItems_.end(); ++it) {
                                if (it->titleId == titleId) {
                                    rawItems_.erase(it);
                                    break;
                                }
                            }
                            rebuildSections();
                            if (recycler && !sections_.empty()) {
                                brls::Application::giveFocus(recycler);
                            }
                        } catch (const std::exception& e) {
                            util::logLine(std::string("library: uninstall UI update exception: ") + e.what());
                        }
                        brls::Application::notify(brls::getStr("app/library/uninstall_success", displayName));
                    } else {
                        brls::Dialog* errDialog = new brls::Dialog("app/library/uninstall_failed"_i18n);
                        errDialog->addButton("app/common/ok"_i18n, []() {});
                        errDialog->open();
                    }
                });
            } catch (const std::exception& e) {
                util::logLine(std::string("library: uninstall async exception: ") + e.what());
            } catch (...) {
                util::logLine("library: uninstall async unknown exception");
            }
        });
    });
    dialog->addButton("app/common/no"_i18n, []() {});
    dialog->open();
}

int LibraryView::LibraryDataSource::numberOfSections(brls::RecyclerFrame* recycler) {
    return parent_->sections_.size();
}

int LibraryView::LibraryDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    if (section >= 0 && section < static_cast<int>(parent_->sections_.size())) {
        return parent_->sections_[section].items.size();
    }
    return 0;
}

float LibraryView::LibraryDataSource::heightForHeader(brls::RecyclerFrame* recycler, int section) {
    if (section >= 0 && section < static_cast<int>(parent_->sections_.size())) {
        return parent_->sections_[section].title.empty() ? 0.0f : 42.0f;
    }
    return 0.0f;
}

std::string LibraryView::LibraryDataSource::titleForHeader(brls::RecyclerFrame* recycler, int section) {
    if (section >= 0 && section < static_cast<int>(parent_->sections_.size())) {
        return parent_->sections_[section].title;
    }
    return "";
}

brls::RecyclerCell* LibraryView::LibraryDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    LibraryRowCell* cell = dynamic_cast<LibraryRowCell*>(recycler->dequeueReusableCell("Row"));
    if (index.section >= parent_->sections_.size() || index.row < 0 ||
        static_cast<size_t>(index.row) >= parent_->sections_[index.section].items.size()) {
        return cell;
    }

    if (cell->imageToken) *(cell->imageToken) = false;
    cell->imageToken = std::make_shared<bool>(true);

    const auto& item = parent_->sections_[index.section].items[index.row];

    if (!cell->title || !cell->titleId || !cell->currentVersion || !cell->latestVersion || !cell->statusBadge || !cell->statusBox || !cell->cover) {
        brls::Logger::error("LibraryDataSource: one or more cell child views are NULL!");
        return cell;
    }

    try {
        cell->title->setText(cleanTitle(item.game.title));
        
        char tidBuf[32];
        sprintf(tidBuf, "%016llX", (unsigned long long)item.titleId);
        std::string tidStrUpper(tidBuf);
        
        char tidBufLower[32];
        sprintf(tidBufLower, "%016llx", (unsigned long long)item.titleId);
        std::string tidStrLower(tidBufLower);

        cell->titleId->setText("ID: " + tidStrUpper);

        if (cell->modBadgeBox && cell->modBadge) {
            if (item.hasMods) {
                cell->modBadgeBox->setVisibility(brls::Visibility::VISIBLE);
                cell->modBadge->setText(item.modDetails.empty() ? "app/library/badge_mods"_i18n : item.modDetails);
            } else {
                cell->modBadgeBox->setVisibility(brls::Visibility::GONE);
            }
        }

        cell->currentVersion->setText(item.currentVersion);
        cell->latestVersion->setText(item.latestVersion);

        // Configure status badge dynamically
        if (item.updateIgnored) {
            cell->statusBadge->setText("app/library/badge_ignored"_i18n);
            cell->statusBadge->setTextColor(nvgRGB(220, 220, 220));
            cell->statusBox->setBackgroundColor(nvgRGB(75, 85, 99)); // Slate
        } else if (item.status == GameUpdateStatus::UpdateAvailable) {
            if (item.hasMods) {
                cell->statusBadge->setText("app/library/badge_update_mod_warn"_i18n);
                cell->statusBadge->setTextColor(nvgRGB(255, 255, 255));
                cell->statusBox->setBackgroundColor(nvgRGB(230, 81, 0)); // Dark warning orange
            } else {
                cell->statusBadge->setText("app/library/badge_update"_i18n);
                cell->statusBadge->setTextColor(nvgRGB(255, 255, 255));
                cell->statusBox->setBackgroundColor(nvgRGB(255, 87, 34)); // Orange/red
            }
        } else if (item.status == GameUpdateStatus::UpToDate) {
            cell->statusBadge->setText("app/library/badge_uptodate"_i18n);
            cell->statusBadge->setTextColor(nvgRGB(255, 255, 255));
            cell->statusBox->setBackgroundColor(nvgRGB(76, 175, 80)); // Green
        } else {
            cell->statusBadge->setText("app/library/badge_unknown"_i18n);
            cell->statusBadge->setTextColor(nvgRGB(255, 255, 255));
            cell->statusBox->setBackgroundColor(nvgRGB(120, 120, 120)); // Gray: no version data
        }

        // Prefer the system icon cached from the installed game's NACP
        std::string localIconPath;
#ifdef __SWITCH__
        {
            char iconPath[128];
            std::snprintf(iconPath, sizeof(iconPath), TSNX_CACHE_ICONS "/%016llX.jpg", (unsigned long long)item.titleId);
            struct stat st;
            if (stat(iconPath, &st) == 0 && st.st_size > 0) localIconPath = iconPath;
        }
#endif
        if (!localIconPath.empty()) {
            cell->cover->setImageFromFile(localIconPath);
        } else {
            std::string coverUrl = item.game.cover;
            std::string fallbackUrl = "https://tinfoil.io/resources/images/icon/" + tidStrLower + ".png";
            
            if (coverUrl.empty()) {
                coverUrl = "https://tinfoil.io/resources/images/icon/" + tidStrLower + ".png";
                fallbackUrl = "https://tinfoil.media/resources/images/icon/" + tidStrLower + ".png";
            }
            
            setImageFromHTTPS(cell->cover, coverUrl, cell->imageToken, "romfs:/img/demo_icon.jpg", false, fallbackUrl);
        }

        Game game = item.game;
        std::string rawName = item.rawName;
        uint64_t tid = item.titleId;
        std::string displayName = item.rawName.empty() ? cleanTitle(item.game.title) : item.rawName;

        // Click action (A button)
        if (item.hasMods && !item.updateIgnored && item.status == GameUpdateStatus::UpdateAvailable) {
            cell->registerClickAction([parent = parent_, item](brls::View* view) {
                parent->showModWarningDialog(item);
                return true;
            });
        } else {
            cell->registerClickAction([game, rawName](brls::View* view) {
                if (!game.magnet.empty()) {
                    brls::Application::pushActivity(new GameDetailView(game));
                } else {
                    std::string query = getFirstTwoWords(rawName);
                    if (query.empty()) query = rawName;
                    brls::Application::pushActivity(new CatalogView(query));
                }
                return true;
            });
        }

        // Toggle update ignore action (X button)
        if (item.updateIgnored) {
            cell->registerAction("app/library/action_enable_update"_i18n, brls::ControllerButton::BUTTON_X,
                [parent = parent_, tid, displayName](brls::View* view) {
                    parent->toggleUpdateIgnored(tid, displayName);
                    return true;
                });
        } else {
            cell->registerAction("app/library/action_disable_update"_i18n, brls::ControllerButton::BUTTON_X,
                [parent = parent_, tid, displayName](brls::View* view) {
                    parent->toggleUpdateIgnored(tid, displayName);
                    return true;
                });
        }

        // Uninstall action (Y button)
        cell->registerAction("app/library/uninstall"_i18n, brls::ControllerButton::BUTTON_Y,
            [parent = parent_, tid, displayName](brls::View* view) {
                parent->uninstallGame(tid, displayName);
                return true;
            });

    } catch (const std::exception& e) {
        brls::Logger::error("LibraryDataSource: EXCEPTION in card setup [{}, {}]: {}", index.section, index.row, e.what());
    } catch (...) {
        brls::Logger::error("LibraryDataSource: UNKNOWN EXCEPTION in card setup [{}, {}]", index.section, index.row);
    }
    
    return cell;
}

} // namespace ui
