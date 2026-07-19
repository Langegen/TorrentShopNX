#include "UpdatesView.hpp"
#include "CatalogView.hpp"
#include "GameDetailView.hpp"
#include "../utils/log.h"
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

static uint32_t getInstalledUpdateVersion(uint64_t baseTitleId) {
    if (baseTitleId == 0) return 0;
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    uint32_t version = 0;
    NcmContentMetaDatabase db;
    uint64_t patchTitleId = baseTitleId | 0x800ULL;
    
    Result rc = ncmInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
        if (R_FAILED(rc)) {
            rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_BuiltInUser);
        }
        if (R_SUCCEEDED(rc)) {
            NcmContentMetaKey key;
            rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, patchTitleId);
            if (R_SUCCEEDED(rc)) {
                version = key.version;
            }
            ncmContentMetaDatabaseClose(&db);
        }
        ncmExit();
    }
    return version;
}
#endif

extern std::vector<Game> g_games;

namespace ui {

UpdateRowCell::UpdateRowCell() {
    this->inflateFromXMLRes("xml/update_row_cell.xml");
}

UpdateRowCell::~UpdateRowCell() {
    if (imageToken) *imageToken = false;
}

UpdateRowCell* UpdateRowCell::create() {
    return new UpdateRowCell();
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

static std::string formatNintendoVersion(uint32_t versionNum, const std::string& currentSemver) {
    if (versionNum < 65536) {
        return "1.0.0";
    }
    if (versionNum % 65536 != 0) {
        return std::to_string(versionNum >> 16) + "." + 
               std::to_string((versionNum >> 8) & 0xFF) + "." + 
               std::to_string(versionNum & 0xFF);
    }
    
    uint32_t idx = versionNum / 65536;
    uint32_t major = 1;
    
    std::stringstream ss(currentSemver);
    std::string part;
    if (std::getline(ss, part, '.')) {
        try { major = std::stoul(part); } catch (...) {}
    }
    
    uint32_t minor = 0;
    uint32_t patch = 0;
    if (idx == 0) {
        minor = 0; patch = 0;
    } else if (idx == 1) {
        minor = 1; patch = 0;
    } else if (idx == 2) {
        minor = 1; patch = 1;
    } else if (idx == 3) {
        minor = 1; patch = 2;
    } else if (idx == 4) {
        minor = 2; patch = 0;
    } else if (idx == 5) {
        minor = 2; patch = 1;
    } else {
        minor = idx / 2;
        patch = idx % 2;
    }
    
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

static bool isSemverNewer(const std::string& latest, const std::string& current) {
    std::vector<int> lParts, cParts;
    std::stringstream lss(latest), css(current);
    std::string part;
    while (std::getline(lss, part, '.')) {
        try { lParts.push_back(std::stoi(part)); } catch (...) { lParts.push_back(0); }
    }
    while (std::getline(css, part, '.')) {
        try { cParts.push_back(std::stoi(part)); } catch (...) { cParts.push_back(0); }
    }
    while (lParts.size() < 3) lParts.push_back(0);
    while (cParts.size() < 3) cParts.push_back(0);
    
    for (size_t i = 0; i < 3; ++i) {
        if (lParts[i] != cParts[i]) {
            return lParts[i] > cParts[i];
        }
    }
    return false;
}

static bool downloadVersionsDatabaseIfNeeded() {
#ifndef __SWITCH__
    std::string path = "./versions.txt";
#else
    std::string path = "sdmc:/switch/TorrentShopNX/versions.txt";
#endif
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
    std::unordered_map<uint64_t, uint32_t> database;
#ifndef __SWITCH__
    std::string path = "./versions.txt";
#else
    std::string path = "sdmc:/switch/TorrentShopNX/versions.txt";
#endif
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
    return database;
}

UpdatesView::UpdatesView() {
    cancelToken_ = std::make_shared<bool>(false);
}

UpdatesView::~UpdatesView() {
    if (cancelToken_) {
        *cancelToken_ = true;
    }
}

void UpdatesView::onContentAvailable() {
    if (recycler) {
        recycler->registerCell("Row", []() { return UpdateRowCell::create(); });
        recycler->setDataSource(new UpdatesDataSource(this));
    }

    scanForUpdates();
}

void UpdatesView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState && recycler && !displayItems_.empty()) {
        brls::Application::giveFocus(recycler);
    }
}

void UpdatesView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

void UpdatesView::scanForUpdates() {
    if (isScanning_) {
        util::logLine("updates: scan already in progress");
        return;
    }
    isScanning_ = true;
    
    if (statsHint) {
        statsHint->setText("Загрузка базы версий titledb...");
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
                    statsHint->setText("Ошибка: не удалось загрузить titledb. Подключите интернет.");
                }
                isScanning_ = false;
            });
            return;
        }
        
        brls::sync([this, cancelToken]() {
            if (cancelToken && *cancelToken) return;
            if (statsHint) {
                statsHint->setText("Сканирование установленных игр...");
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
                }
                installedList.push_back({tid, name, curVer});
            }
            nsExit();
        }
#else
        // Mock data for PC testing
        installedList.push_back({0x0100000000010000ULL, "Super Mario Odyssey", "1.0.0"});
        installedList.push_back({0x01007ef00011e000ULL, "The Legend of Zelda: Breath of the Wild", "1.0.0"});
        installedList.push_back({0x0100000000020000ULL, "Unmatched Dummy Game", "1.1.0"});
#endif

        if (cancelToken && *cancelToken) return;

        std::vector<UpdateItem> displayItems;

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
#ifdef __SWITCH__
            currentVer = getInstalledUpdateVersion(baseTid);
#endif
            
            bool needsUpdate = false;
            std::string latestVerStr = inst.currentVersionStr;
            
            if (latestVer > 0) {
                std::string estLatestVerStr = formatNintendoVersion(latestVer, inst.currentVersionStr);
                if (currentVer > 0) {
                    needsUpdate = (latestVer > currentVer);
                } else {
                    needsUpdate = isSemverNewer(estLatestVerStr, inst.currentVersionStr);
                }
                if (needsUpdate) {
                    latestVerStr = estLatestVerStr;
                }
            }
            
            // Match strictly by Title ID parsed from catalog brackets
            bool foundInCatalog = false;
            for (const auto& g : g_games) {
                uint64_t catalogTid = parseTitleIdFromString(g.title);
                if (catalogTid != 0 && (catalogTid == baseTid || catalogTid == patchTid)) {
                    UpdateItem item;
                    item.game = g;
                    item.currentVersion = inst.currentVersionStr;
                    item.latestVersion = latestVerStr;
                    item.needsUpdate = needsUpdate;
                    item.titleId = baseTid;
                    item.rawName = inst.name;
                    displayItems.push_back(item);
                    foundInCatalog = true;
                    break;
                }
            }
            
            // Fallback to name matching if Title ID match failed
            if (!foundInCatalog) {
                std::string instClean = cleanNameForMatching(inst.name);
                for (const auto& g : g_games) {
                    std::string catClean = cleanNameForMatching(g.title);
                    if (!instClean.empty() && !catClean.empty() && 
                        (instClean == catClean || catClean.find(instClean) != std::string::npos || instClean.find(catClean) != std::string::npos)) {
                        UpdateItem item;
                        item.game = g;
                        item.currentVersion = inst.currentVersionStr;
                        item.latestVersion = latestVerStr;
                        item.needsUpdate = needsUpdate;
                        item.titleId = baseTid;
                        item.rawName = inst.name;
                        displayItems.push_back(item);
                        foundInCatalog = true;
                        break;
                    }
                }
            }
            
            // If not found in catalog, create a dummy item
            if (!foundInCatalog) {
                UpdateItem item;
                item.game.title = inst.name.empty() ? ("Unknown Game") : inst.name;
                item.game.cover = "";
                item.game.size = "";
                item.currentVersion = inst.currentVersionStr;
                item.latestVersion = latestVerStr;
                item.needsUpdate = needsUpdate;
                item.titleId = baseTid;
                item.rawName = inst.name;
                displayItems.push_back(item);
            }
        }
        
        if (cancelToken && *cancelToken) return;

        // Sort items: needsUpdate first, then alphabetically by title
        std::sort(displayItems.begin(), displayItems.end(), [](const UpdateItem& a, const UpdateItem& b) {
            if (a.needsUpdate != b.needsUpdate) {
                return a.needsUpdate > b.needsUpdate; // true comes first
            }
            return a.game.title < b.game.title;
        });
        
        if (cancelToken && *cancelToken) return;

        // Sync with UI thread
        brls::sync([this, cancelToken, items = std::move(displayItems)]() {
            if (cancelToken && *cancelToken) return;

            isScanning_ = false;
            if (!this->getContentView()) return;
            
            displayItems_ = std::move(items);
            
            int updateCount = 0;
            for (const auto& item : displayItems_) {
                if (item.needsUpdate) {
                    updateCount++;
                }
            }
            
            if (statsHint) {
                statsHint->setText("Доступно обновлений: " + std::to_string(updateCount) + " / Всего игр: " + std::to_string(displayItems_.size()));
            }
            if (recycler) {
                recycler->reloadData();
            }
            
            if (!displayItems_.empty() && recycler) {
                brls::Application::giveFocus(recycler);
            }
        });
    });
}

int UpdatesView::UpdatesDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return parent_->displayItems_.size();
}

brls::RecyclerCell* UpdatesView::UpdatesDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    UpdateRowCell* cell = dynamic_cast<UpdateRowCell*>(recycler->dequeueReusableCell("Row"));
    if (!cell) return nullptr;

    int row = index.row;
    
    if (cell->imageToken) *(cell->imageToken) = false;
    cell->imageToken = std::make_shared<bool>(true);

    const auto& item = parent_->displayItems_[row];

    if (!cell->title || !cell->titleId || !cell->currentVersion || !cell->latestVersion || !cell->statusBadge || !cell->statusBox || !cell->cover) {
        brls::Logger::error("UpdatesDataSource: one or more cell child views are NULL!");
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

        cell->currentVersion->setText(item.currentVersion);
        cell->latestVersion->setText(item.latestVersion);

        // Configure status badge dynamically
        if (item.needsUpdate) {
            cell->statusBadge->setText("ОБНОВИТЬ");
            cell->statusBadge->setTextColor(nvgRGB(255, 255, 255));
            cell->statusBox->setBackgroundColor(nvgRGB(255, 87, 34)); // Orange/red
        } else {
            cell->statusBadge->setText("АКТУАЛЬНО");
            cell->statusBadge->setTextColor(nvgRGB(255, 255, 255));
            cell->statusBox->setBackgroundColor(nvgRGB(76, 175, 80)); // Green
        }

        // Fetch cover: use catalog cover URL if available, otherwise fallback to tinfoil.io and tinfoil.media CDN by Title ID
        std::string coverUrl = item.game.cover;
        std::string fallbackUrl = "https://tinfoil.io/resources/images/icon/" + tidStrLower + ".png";
        
        if (coverUrl.empty()) {
            coverUrl = "https://tinfoil.io/resources/images/icon/" + tidStrLower + ".png";
            fallbackUrl = "https://tinfoil.media/resources/images/icon/" + tidStrLower + ".png";
        }
        
        setImageFromHTTPS(cell->cover, coverUrl, cell->imageToken, "romfs:/img/demo_icon.jpg", false, fallbackUrl);

        Game game = item.game;
        std::string rawName = item.rawName;
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

    } catch (const std::exception& e) {
        brls::Logger::error("UpdatesDataSource: EXCEPTION in card setup {}: {}", row, e.what());
    } catch (...) {
        brls::Logger::error("UpdatesDataSource: UNKNOWN EXCEPTION in card setup {}", row);
    }
    
    return cell;
}

} // namespace ui
