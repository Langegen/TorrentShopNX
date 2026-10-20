#include "LibraryView.hpp"
#include "ThemeManager.hpp"
#include "CatalogView.hpp"
#include "GameDetailView.hpp"
#include "../utils/log.h"
#include "../utils/switch_utils.h"
#include "../utils/app_paths.h"
#include "../utils/mod_utils.h"
#include "../utils/file_ops.h"
#include "../catalog/IgnoredUpdatesManager.hpp"
#include "../installer/ncz_parser.h"
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

// Queries both patch version and running SDK version for installed titles in one NCM session.
static void getInstalledTitleMetadata(
    const std::vector<uint64_t>& baseTids,
    std::unordered_map<uint64_t, uint32_t>& out_patch_versions,
    std::unordered_map<uint64_t, std::string>& out_sdk_versions) {
    if (baseTids.empty()) return;

    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return;

    NcmContentMetaDatabase dbSd = {}, dbNand = {};
    bool hasSdDb = R_SUCCEEDED(ncmOpenContentMetaDatabase(&dbSd, NcmStorageId_SdCard));
    bool hasNandDb = R_SUCCEEDED(ncmOpenContentMetaDatabase(&dbNand, NcmStorageId_BuiltInUser));

    NcmContentStorage csSd = {}, csNand = {};
    bool hasSdCs = R_SUCCEEDED(ncmOpenContentStorage(&csSd, NcmStorageId_SdCard));
    bool hasNandCs = R_SUCCEEDED(ncmOpenContentStorage(&csNand, NcmStorageId_BuiltInUser));

    uint8_t header_key[0x20] = {};
    bool have_key = installer::deriveNcaHeaderKey(header_key);

    auto queryTidKey = [&](uint64_t tid, NcmContentMetaKey& out_key, NcmStorageId& out_storage) -> bool {
        if (hasSdDb && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&dbSd, &out_key, tid))) {
            out_storage = NcmStorageId_SdCard;
            return true;
        }
        if (hasNandDb && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&dbNand, &out_key, tid))) {
            out_storage = NcmStorageId_BuiltInUser;
            return true;
        }
        return false;
    };

    auto readSdkForMetaKey = [&](const NcmContentMetaKey& metaKey, NcmStorageId storageId) -> std::string {
        if (!have_key) return "";
        NcmContentMetaDatabase* db = (storageId == NcmStorageId_SdCard) ? &dbSd : &dbNand;
        NcmContentId cid = {};
        if (R_FAILED(ncmContentMetaDatabaseGetContentIdByType(db, &cid, &metaKey, NcmContentType_Program))) {
            return "";
        }

        NcmContentStorage* primaryCs = (storageId == NcmStorageId_SdCard) ? &csSd : &csNand;
        NcmContentStorage* secondaryCs = (storageId == NcmStorageId_SdCard) ? &csNand : &csSd;

        bool has = false;
        NcmContentStorage* targetCs = nullptr;
        if (primaryCs && R_SUCCEEDED(ncmContentStorageHas(primaryCs, &has, &cid)) && has) {
            targetCs = primaryCs;
        } else if (secondaryCs && R_SUCCEEDED(ncmContentStorageHas(secondaryCs, &has, &cid)) && has) {
            targetCs = secondaryCs;
        }
        if (!targetCs) return "";

        uint8_t header_buf[0x4000] = {};
        if (R_FAILED(ncmContentStorageReadContentIdFile(targetCs, header_buf, sizeof(header_buf), &cid, 0))) {
            return "";
        }

        installer::NcaHeader header = {};
        if (installer::decryptNcaHeader(header_buf, header_key, header)) {
            util::SdkVersion sdk = util::SdkVersion::parse(header.sdk_addon_version);
            if (sdk.valid) {
                return sdk.toString();
            }
        }
        return "";
    };

    for (uint64_t baseTid : baseTids) {
        uint64_t patchTid = baseTid | 0x800ULL;
        NcmContentMetaKey patchKey = {};
        NcmStorageId patchStorage = NcmStorageId_SdCard;
        bool hasPatch = queryTidKey(patchTid, patchKey, patchStorage);

        if (hasPatch) {
            out_patch_versions[baseTid] = patchKey.version;
            std::string sdkStr = readSdkForMetaKey(patchKey, patchStorage);
            if (!sdkStr.empty()) {
                out_sdk_versions[baseTid] = sdkStr;
                continue;
            }
        }

        NcmContentMetaKey baseKey = {};
        NcmStorageId baseStorage = NcmStorageId_SdCard;
        if (queryTidKey(baseTid, baseKey, baseStorage)) {
            std::string sdkStr = readSdkForMetaKey(baseKey, baseStorage);
            if (!sdkStr.empty()) {
                out_sdk_versions[baseTid] = sdkStr;
            }
        }
    }

    if (hasSdCs) ncmContentStorageClose(&csSd);
    if (hasNandCs) ncmContentStorageClose(&csNand);
    if (hasSdDb) ncmContentMetaDatabaseClose(&dbSd);
    if (hasNandDb) ncmContentMetaDatabaseClose(&dbNand);
    ncmExit();
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

struct GameComponentData {
    uint64_t totalSize = 0;
    uint64_t appSize = 0;
    uint64_t patchSize = 0;
    uint64_t dlcSize = 0;
    int dlcCount = 0;
    std::vector<InstalledComponent> components;
};

static uint64_t calculateDirectorySize(const std::string& path) {
    uint64_t total = 0;
    try {
        if (!path.empty() && std::filesystem::exists(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    total += entry.file_size();
                }
            }
        }
    } catch (...) {}
    return total;
}

static bool deleteGameMods(uint64_t titleId, const std::string& modPath) {
    bool deletedAny = false;
    std::string err;
    if (!modPath.empty() && std::filesystem::exists(modPath)) {
        if (util::deletePathRecursive(modPath, err)) {
            deletedAny = true;
        }
    }
    char tidUpper[32], tidLower[32];
    std::snprintf(tidUpper, sizeof(tidUpper), "%016llX", (unsigned long long)titleId);
    std::snprintf(tidLower, sizeof(tidLower), "%016llx", (unsigned long long)titleId);
    std::vector<std::string> candidateDirs = {
#ifdef __SWITCH__
        std::string("sdmc:/atmosphere/contents/") + tidLower,
        std::string("sdmc:/atmosphere/contents/") + tidUpper,
        std::string("sdmc:/atmosphere/titles/") + tidLower,
        std::string("sdmc:/atmosphere/titles/") + tidUpper,
#else
        std::string("atmosphere/contents/") + tidUpper,
        std::string("atmosphere/contents/") + tidLower,
        std::string("sdmc/atmosphere/contents/") + tidUpper,
        std::string("sdmc/atmosphere/contents/") + tidLower,
#endif
    };
    for (const auto& d : candidateDirs) {
        if (std::filesystem::exists(d)) {
            if (util::deletePathRecursive(d, err)) deletedAny = true;
        }
    }
    return deletedAny;
}

#ifdef __SWITCH__
static bool deleteNcmComponent(uint8_t storageId, const NcmContentMetaKey& metaKey) {
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return false;

    auto tryDeleteFromDb = [&](NcmStorageId stId) -> bool {
        NcmContentMetaDatabase db = {};
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, stId))) return false;

        bool hasMeta = false;
        if (R_FAILED(ncmContentMetaDatabaseHas(&db, &hasMeta, &metaKey)) || !hasMeta) {
            ncmContentMetaDatabaseClose(&db);
            return false;
        }

        s32 entriesWritten = 0;
        NcmContentInfo contentInfos[32] = {};
        if (R_SUCCEEDED(ncmContentMetaDatabaseListContentInfo(&db, &entriesWritten, contentInfos, 32, &metaKey, 0))) {
            NcmContentStorage cs = {};
            if (R_SUCCEEDED(ncmOpenContentStorage(&cs, stId))) {
                for (s32 i = 0; i < entriesWritten; ++i) {
                    ncmContentStorageDelete(&cs, &contentInfos[i].content_id);
                }
                ncmContentStorageClose(&cs);
            }
        }

        Result remRc = ncmContentMetaDatabaseRemove(&db, &metaKey);
        if (R_SUCCEEDED(remRc)) {
            ncmContentMetaDatabaseCommit(&db);
        }
        ncmContentMetaDatabaseClose(&db);
        return R_SUCCEEDED(remRc);
    };

    bool ok = tryDeleteFromDb(static_cast<NcmStorageId>(storageId));
    if (!ok) {
        // Fallback to alternate storage
        NcmStorageId altStorage = (storageId == NcmStorageId_SdCard) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
        ok = tryDeleteFromDb(altStorage);
    }
    ncmExit();

    Result nsRc = nsInitialize();
    if (R_SUCCEEDED(nsRc)) {
        nsDeleteRedundantApplicationEntity();
        nsExit();
    }
    return ok;
}

static void getInstalledComponentsAndSizes(
    const std::vector<uint64_t>& baseTids,
    std::unordered_map<uint64_t, GameComponentData>& out_data) {
    if (baseTids.empty()) return;

    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    Result nsRc = nsInitialize();
    Result ncmRc = ncmInitialize();

    NcmContentMetaDatabase dbSd = {}, dbNand = {};
    bool hasSdDb = R_SUCCEEDED(ncmOpenContentMetaDatabase(&dbSd, NcmStorageId_SdCard));
    bool hasNandDb = R_SUCCEEDED(ncmOpenContentMetaDatabase(&dbNand, NcmStorageId_BuiltInUser));

    struct ApplicationOccupiedSizeEntity {
        uint8_t storage_id;
        uint8_t reserved[7];
        uint64_t app_size;
        uint64_t patch_size;
        uint64_t aoc_size;
    };

    for (uint64_t tid : baseTids) {
        GameComponentData gcd;
        if (R_SUCCEEDED(nsRc)) {
            NsApplicationOccupiedSize occSize = {};
            if (R_SUCCEEDED(nsCalculateApplicationOccupiedSize(tid, &occSize))) {
                const auto* entities = reinterpret_cast<const ApplicationOccupiedSizeEntity*>(&occSize);
                for (int e = 0; e < 4; ++e) {
                    gcd.appSize += entities[e].app_size;
                    gcd.patchSize += entities[e].patch_size;
                    gcd.dlcSize += entities[e].aoc_size;
                }
                gcd.totalSize = gcd.appSize + gcd.patchSize + gcd.dlcSize;
            }

            s32 metaCount = 0;
            if (R_SUCCEEDED(nsCountApplicationContentMeta(tid, &metaCount)) && metaCount > 0) {
                std::vector<NsApplicationContentMetaStatus> metaList(metaCount);
                s32 outCount = 0;
                if (R_SUCCEEDED(nsListApplicationContentMetaStatus(tid, 0, metaList.data(), metaCount, &outCount))) {
                    metaList.resize(outCount);
                    int dlcIdx = 1;
                    for (const auto& cms : metaList) {
                        InstalledComponent comp;
                        comp.titleId = cms.application_id;
                        comp.version = cms.version;
                        comp.storageId = cms.storageID;

                        NcmContentMetaDatabase* db = (cms.storageID == NcmStorageId_SdCard) ? &dbSd : &dbNand;
                        bool dbValid = (cms.storageID == NcmStorageId_SdCard) ? hasSdDb : hasNandDb;
                        if (dbValid) {
                            NcmContentMetaKey key = {};
                            key.id = cms.application_id;
                            key.version = cms.version;
                            key.type = static_cast<NcmContentMetaType>(cms.meta_type);
                            key.install_type = NcmContentInstallType_Full;
                            u64 cSize = 0;
                            if (R_SUCCEEDED(ncmContentMetaDatabaseGetSize(db, &cSize, &key))) {
                                comp.sizeBytes = cSize;
                            }
                        }

                        if (cms.meta_type == NcmContentMetaType_Application) {
                            comp.type = InstalledComponent::Type::BaseGame;
                            comp.displayName = "app/library/comp_base_game"_i18n;
                            if (comp.sizeBytes == 0) comp.sizeBytes = gcd.appSize;
                            gcd.components.push_back(comp);
                        } else if (cms.meta_type == NcmContentMetaType_Patch) {
                            comp.type = InstalledComponent::Type::Patch;
                            comp.displayName = brls::getStr("app/library/comp_patch", "v" + std::to_string(cms.version));
                            if (comp.sizeBytes == 0) comp.sizeBytes = gcd.patchSize;
                            gcd.components.push_back(comp);
                        } else if (cms.meta_type == NcmContentMetaType_AddOnContent) {
                            comp.type = InstalledComponent::Type::Dlc;
                            comp.displayName = brls::getStr("app/library/comp_dlc", std::to_string(dlcIdx++));
                            gcd.dlcCount++;
                            gcd.components.push_back(comp);
                        }
                    }
                }
            }
        }
        out_data[tid] = std::move(gcd);
    }

    if (hasSdDb) ncmContentMetaDatabaseClose(&dbSd);
    if (hasNandDb) ncmContentMetaDatabaseClose(&dbNand);
    if (R_SUCCEEDED(ncmRc)) ncmExit();
    if (R_SUCCEEDED(nsRc)) nsExit();
}
#endif


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
    replaceAll(lower, "+", "plus");
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
    std::string clean;
    for (char c : str) {
        if (c == ':' || c == '-' || c == ',' || c == '.' || c == '!' || c == '?' || c == '\'' || c == '\"') {
            clean.push_back(' ');
        } else {
            clean.push_back(c);
        }
    }
    std::stringstream ss(clean);
    std::string word1, word2;
    if (ss >> word1) {
        if (ss >> word2) {
            return word1 + " " + word2;
        }
        return word1;
    }
    return "";
}

static std::vector<std::string> tokenizeNameForMatching(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    replaceAll(lower, "&", "and");
    replaceAll(lower, "+", "plus");

    std::string noTags;
    noTags.reserve(lower.size());
    bool inBracket = false;
    for (char c : lower) {
        if (c == '[' || c == '{') inBracket = true;
        else if (c == ']' || c == '}') inBracket = false;
        else if (!inBracket) noTags.push_back(c);
    }

    std::vector<std::string> tokens;
    std::string current;
    for (char c : noTags) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(c);
        } else {
            if (!current.empty()) {
                if (current != "repack" && current != "nsz" && current != "nsp" &&
                    current != "xci" && current != "xcz" && current != "rus" &&
                    current != "eng" && current != "multi" && current != "mod") {
                    tokens.push_back(current);
                }
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        if (current != "repack" && current != "nsz" && current != "nsp" &&
            current != "xci" && current != "xcz" && current != "rus" &&
            current != "eng" && current != "multi" && current != "mod") {
            tokens.push_back(current);
        }
    }
    return tokens;
}

static bool tokensMatchGame(const std::vector<std::string>& instTokens, const std::vector<std::string>& catTokens) {
    if (instTokens.empty() || catTokens.empty()) return false;
    if (instTokens == catTokens) return true;
    if (instTokens.size() == 1) {
        const std::string& t = instTokens[0];
        bool inCat = false;
        for (const auto& ct : catTokens) {
            if (ct == t) { inCat = true; break; }
        }
        return inCat && (catTokens.size() <= 3 || t.length() >= 5);
    }
    std::vector<std::string> sigInst;
    for (const auto& t : instTokens) {
        if (t.length() > 1) sigInst.push_back(t);
    }
    if (sigInst.empty()) sigInst = instTokens;
    for (const auto& t : sigInst) {
        bool found = false;
        for (const auto& ct : catTokens) {
            if (ct == t) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
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
        try {
            std::filesystem::path p(path);
            if (p.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(p.parent_path(), ec);
            }
        } catch (...) {}

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

void LibraryView::rebuildSections(int targetSection, int targetRow) {
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
    sortAlpha(upToDateItems);
    sortAlpha(otherItems);
    sortAlpha(ignoredItems);

    if (!modsUpdates.empty()) {
        std::string title = brls::getStr("app/library/section_mods_updates", std::to_string(modsUpdates.size()));
        sections_.push_back({title, std::move(modsUpdates)});
    }
    if (!normalUpdates.empty()) {
        std::string title = brls::getStr("app/library/section_updates", std::to_string(normalUpdates.size()));
        sections_.push_back({title, std::move(normalUpdates)});
    }
    if (!upToDateItems.empty()) {
        std::string title = brls::getStr("app/library/section_uptodate", std::to_string(upToDateItems.size()));
        sections_.push_back({title, std::move(upToDateItems)});
    }
    if (!otherItems.empty()) {
        std::string title = brls::getStr("app/library/section_other", std::to_string(otherItems.size()));
        sections_.push_back({title, std::move(otherItems)});
    }
    if (!ignoredItems.empty()) {
        std::string title = brls::getStr("app/library/section_ignored", std::to_string(ignoredItems.size()));
        sections_.push_back({title, std::move(ignoredItems)});
    }

    if (recycler) {
        brls::IndexPath focusPath(0, 0);
        if (targetSection >= 0 && targetRow >= 0 && !sections_.empty()) {
            size_t sec = static_cast<size_t>(targetSection);
            if (sec >= sections_.size()) sec = sections_.size() - 1;
            int maxRow = static_cast<int>(sections_[sec].items.size()) - 1;
            int r = targetRow;
            if (r > maxRow) r = maxRow;
            if (r < 0) r = 0;
            focusPath = brls::IndexPath(sec, static_cast<size_t>(r));
        }
        recycler->setDefaultCellFocus(focusPath);
        recycler->reloadData();
        if (!sections_.empty()) {
            recycler->setDefaultCellFocus(focusPath);
            recycler->selectRowAt(focusPath, false);
            brls::Application::giveFocus(recycler);
        }
    }
    updateStatsAndSpace();
}

void LibraryView::toggleUpdateIgnored(uint64_t titleId, const std::string& displayName, int currentSection, int currentRow) {
    bool nowIgnored = catalog::IgnoredUpdatesManager::instance().toggleIgnored(titleId);
    rebuildSections(currentSection, currentRow);
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

    dialog->addButton("app/library/mod_warn_btn_proceed"_i18n, [game, rawName, tid]() {
        if (!game.magnet.empty()) {
            brls::Application::pushActivity(new GameDetailView(game));
        } else {
            std::string query;
            if (tid != 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
                query = buf;
            } else {
                query = getFirstTwoWords(rawName);
                if (query.empty()) query = rawName;
            }
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
    if (spaceHint) {
        int64_t sdFree = 0;
        int64_t nandFree = 0;
        bool sdOk = util::getStorageFreeSpace(1, sdFree);
        bool nandOk = util::getStorageFreeSpace(0, nandFree);

        std::string sdStr = sdOk ? formatBytes(static_cast<unsigned long long>(sdFree)) : "app/library/space_unknown"_i18n;
        std::string nandStr = nandOk ? formatBytes(static_cast<unsigned long long>(nandFree)) : "app/library/space_unknown"_i18n;

        spaceHint->setText(brls::getStr("app/library/space", sdStr, nandStr));
        spaceHint->setTextColor(ThemeManager::instance().getAccentColor());
    }

    if (consoleSdkHint) {
        auto cSdk = util::getConsoleSdkVersion();
        if (cSdk.valid) {
            consoleSdkHint->setText(brls::getStr("app/library/console_sdk", cSdk.toString()));
        } else {
            consoleSdkHint->setText("");
        }
    }
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

        // Query installed patch versions, running SDK versions, occupied sizes and components
        std::unordered_map<uint64_t, uint32_t> installedPatchVersions;
        std::unordered_map<uint64_t, std::string> installedSdkVersions;
        getInstalledTitleMetadata(baseTids, installedPatchVersions, installedSdkVersions);

        std::unordered_map<uint64_t, GameComponentData> installedCompData;
        getInstalledComponentsAndSizes(baseTids, installedCompData);
#else
        // Mock data for PC testing
        installedList.push_back({0x0100000000010000ULL, "Super Mario Odyssey", "1.0.0"});
        installedList.push_back({0x01007ef00011e000ULL, "The Legend of Zelda: Breath of the Wild", "1.0.0"});
        installedList.push_back({0x0100000000020000ULL, "Unmatched Dummy Game", "1.1.0"});
        std::unordered_map<uint64_t, uint32_t> installedPatchVersions;
        std::unordered_map<uint64_t, std::string> installedSdkVersions;
        installedSdkVersions[0x0100000000010000ULL] = "17.0.0";
        installedSdkVersions[0x01007ef00011e000ULL] = "18.1.0";
        installedSdkVersions[0x0100000000020000ULL] = "16.0.3";
        installedPatchVersions[0x0100000000010000ULL] = 131072;
        installedPatchVersions[0x01007ef00011e000ULL] = 655360;

        std::unordered_map<uint64_t, GameComponentData> installedCompData;
        {
            // Mario
            GameComponentData marioGcd;
            marioGcd.appSize = 5700000000ULL;
            marioGcd.patchSize = 350000000ULL;
            marioGcd.dlcSize = 520000000ULL;
            marioGcd.totalSize = marioGcd.appSize + marioGcd.patchSize + marioGcd.dlcSize;
            marioGcd.dlcCount = 2;
            marioGcd.components.push_back({InstalledComponent::Type::BaseGame, 0x0100000000010000ULL, 0, marioGcd.appSize, "app/library/comp_base_game"_i18n, 1, ""});
            marioGcd.components.push_back({InstalledComponent::Type::Patch, 0x0100000000010800ULL, 131072, marioGcd.patchSize, "Обновление v131072", 1, ""});
            marioGcd.components.push_back({InstalledComponent::Type::Dlc, 0x0100000000010001ULL, 0, 320000000ULL, "DLC #1", 1, ""});
            marioGcd.components.push_back({InstalledComponent::Type::Dlc, 0x0100000000010002ULL, 0, 200000000ULL, "DLC #2", 1, ""});
            installedCompData[0x0100000000010000ULL] = std::move(marioGcd);

            // Zelda
            GameComponentData zeldaGcd;
            zeldaGcd.appSize = 14400000000ULL;
            zeldaGcd.patchSize = 2500000000ULL;
            zeldaGcd.dlcSize = 2300000000ULL;
            zeldaGcd.totalSize = zeldaGcd.appSize + zeldaGcd.patchSize + zeldaGcd.dlcSize;
            zeldaGcd.dlcCount = 2;
            zeldaGcd.components.push_back({InstalledComponent::Type::BaseGame, 0x01007ef00011e000ULL, 0, zeldaGcd.appSize, "app/library/comp_base_game"_i18n, 1, ""});
            zeldaGcd.components.push_back({InstalledComponent::Type::Patch, 0x01007ef00011e800ULL, 655360, zeldaGcd.patchSize, "Обновление v655360", 1, ""});
            zeldaGcd.components.push_back({InstalledComponent::Type::Dlc, 0x01007ef00011e001ULL, 0, 800000000ULL, "DLC #1", 1, ""});
            zeldaGcd.components.push_back({InstalledComponent::Type::Dlc, 0x01007ef00011e002ULL, 0, 1500000000ULL, "DLC #2", 1, ""});
            installedCompData[0x01007ef00011e000ULL] = std::move(zeldaGcd);

            // Dummy
            GameComponentData dummyGcd;
            dummyGcd.appSize = 1200000000ULL;
            dummyGcd.totalSize = dummyGcd.appSize;
            dummyGcd.components.push_back({InstalledComponent::Type::BaseGame, 0x0100000000020000ULL, 0, dummyGcd.appSize, "app/library/comp_base_game"_i18n, 1, ""});
            installedCompData[0x0100000000020000ULL] = std::move(dummyGcd);
        }
#endif

        if (cancelToken && *cancelToken) return;

        // Build a catalog index once per scan: exact title id lookup + a
        // pre-normalized name list, so per-game matching is O(1) instead of
        // re-running the expensive normalization for every catalog entry.
        auto catalog = getCatalogSnapshot();
        struct CatalogIndex {
            std::unordered_map<uint64_t, const Game*> byTid;
            std::vector<std::pair<std::string, const Game*>> byCleanName;
            std::vector<std::pair<std::vector<std::string>, const Game*>> byTokens;
        };
        CatalogIndex catIndex;
        catIndex.byTid.reserve(catalog->size());
        catIndex.byCleanName.reserve(catalog->size());
        catIndex.byTokens.reserve(catalog->size());
        for (const auto& g : *catalog) {
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
            auto tokens = tokenizeNameForMatching(g.title);
            if (!tokens.empty()) catIndex.byTokens.emplace_back(std::move(tokens), &g);
        }

        auto makeItem = [](const Game& g, const std::string& currentVer,
                           const std::string& latestVer, GameUpdateStatus status,
                           uint64_t baseTid, const std::string& rawName,
                           bool hasMods, bool updateIgnored, const std::string& modDetails,
                           const std::string& modPath,
                           const std::string& sdkVersion,
                           const GameComponentData& gcd,
                           uint32_t installedPatchVer) {
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
            item.modPath = modPath;
            item.sdkVersion = sdkVersion;

            item.totalSizeBytes = gcd.totalSize;
            item.appSizeBytes = gcd.appSize;
            item.patchSizeBytes = gcd.patchSize;
            item.dlcSizeBytes = gcd.dlcSize;
            item.dlcCount = gcd.dlcCount;
            item.hasPatch = (installedPatchVer > 0 || gcd.patchSize > 0);
            item.installedPatchVersion = installedPatchVer;
            item.components = gcd.components;

            if (item.hasMods) {
                item.modsSizeBytes = calculateDirectorySize(item.modPath);
                item.totalSizeBytes += item.modsSizeBytes;
                InstalledComponent modComp;
                modComp.type = InstalledComponent::Type::Mods;
                modComp.titleId = baseTid;
                modComp.sizeBytes = item.modsSizeBytes;
                modComp.displayName = brls::getStr("app/library/comp_mods", item.modDetails.empty() ? "RomFS" : item.modDetails);
                modComp.extraInfo = item.modPath;
                item.components.push_back(modComp);
            }

            return item;
        };

        std::vector<LibraryItem> displayItems;

        // Match installed items with the catalog (g_games) using titledb
        for (const auto& inst : installedList) {
            if (cancelToken && *cancelToken) return;

            uint64_t baseTid = inst.titleId;
            GameComponentData gcd;
            {
                auto gcit = installedCompData.find(baseTid);
                if (gcit != installedCompData.end()) gcd = gcit->second;
            }
            uint64_t patchTid = baseTid | 0x800ULL; // Patch ID
            
            std::string sdkVersion;
            auto sit = installedSdkVersions.find(baseTid);
            if (sit != installedSdkVersions.end()) {
                sdkVersion = sit->second;
            }

            uint32_t latestVer = 0;
            bool foundVersionInDb = false;
            auto it = availableVersions.find(patchTid);
            if (it != availableVersions.end()) {
                latestVer = it->second;
                foundVersionInDb = true;
            } else {
                auto bit = availableVersions.find(baseTid);
                if (bit != availableVersions.end()) {
                    latestVer = bit->second;
                    foundVersionInDb = true;
                }
            }
            
            uint32_t currentVer = 0;
            auto iv = installedPatchVersions.find(baseTid);
            if (iv != installedPatchVersions.end()) {
                currentVer = iv->second;
            }
            
            // The raw titledb value (e.g. 196608) is a Nintendo "update number"
            // that cannot be reliably converted to real semver (1.2.0 style),
            // so display it honestly as v<number> — same convention tinfoil uses.
            bool hasVersionInfo = (latestVer > 0) || foundVersionInDb;
            GameUpdateStatus status = GameUpdateStatus::Unknown;
            std::string latestVerStr = "—";
            std::string currentVerStr = inst.currentVersionStr;
            if (currentVer > 0) {
                currentVerStr = "v" + std::to_string(currentVer);
            }
            
            if (hasVersionInfo) {
                if (latestVer > 0) {
                    latestVerStr = "v" + std::to_string(latestVer);
                    bool needsUpdate = false;
                    if (currentVer > 0) {
                        needsUpdate = (latestVer > currentVer);
                    } else {
                        needsUpdate = true; // an update exists and none is installed
                    }
                    status = needsUpdate ? GameUpdateStatus::UpdateAvailable : GameUpdateStatus::UpToDate;
                } else {
                    // Base game exists in titledb with no published patches yet (version 0)
                    latestVerStr = currentVerStr.empty() ? "v0" : currentVerStr;
                    status = GameUpdateStatus::UpToDate;
                }
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
                    displayItems.push_back(makeItem(*mit->second, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary, modInfo.modPath, sdkVersion, gcd, currentVer));
                    foundInCatalog = true;
                }
            }
            
            // Fallback to name matching if Title ID match failed
            if (!foundInCatalog) {
                std::string instClean = cleanNameForMatching(inst.name);
                auto instTokens = tokenizeNameForMatching(inst.name);
                if (!instClean.empty()) {
                    // 1. Try exact clean name match first
                    for (const auto& entry : catIndex.byCleanName) {
                        if (entry.first == instClean) {
                            displayItems.push_back(makeItem(*entry.second, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary, modInfo.modPath, sdkVersion, gcd, currentVer));
                            foundInCatalog = true;
                            break;
                        }
                    }
                    // 2. Try token-based matching (e.g. multi-packs like Shovel Knight: Treasure Trove)
                    if (!foundInCatalog && !instTokens.empty()) {
                        for (const auto& entry : catIndex.byTokens) {
                            if (tokensMatchGame(instTokens, entry.first)) {
                                displayItems.push_back(makeItem(*entry.second, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary, modInfo.modPath, sdkVersion, gcd, currentVer));
                                foundInCatalog = true;
                                break;
                            }
                        }
                    }
                    // 3. Fallback to substring matching ONLY if strings are >= 4 characters
                    if (!foundInCatalog && instClean.length() >= 4) {
                        for (const auto& entry : catIndex.byCleanName) {
                            const std::string& catClean = entry.first;
                            if (catClean.length() >= 4 && (catClean.find(instClean) != std::string::npos || (catClean.length() >= 8 && instClean.find(catClean) != std::string::npos))) {
                                displayItems.push_back(makeItem(*entry.second, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary, modInfo.modPath, sdkVersion, gcd, currentVer));
                                foundInCatalog = true;
                                break;
                            }
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
                displayItems.push_back(makeItem(dummy, currentVerStr, latestVerStr, status, baseTid, inst.name, modInfo.hasMods, isIgnored, modInfo.summary, modInfo.modPath, sdkVersion, gcd, currentVer));
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

void LibraryView::showGameActionMenu(const LibraryItem& item, int currentSection, int currentRow) {
    std::string displayName = item.rawName.empty() ? cleanTitle(item.game.title) : item.rawName;
    
    brls::Dialog* dialog = new brls::Dialog(brls::getStr("app/library/action_menu_title") + "\n«" + displayName + "»");

    // 1. Manage Components
    dialog->addButton("app/library/action_manage_components"_i18n, [this, item, currentSection, currentRow]() {
        showComponentManager(item, currentSection, currentRow);
    });

    // 2. Delete completely
    dialog->addButton("app/library/action_uninstall_all"_i18n, [this, item, displayName, currentSection, currentRow]() {
        if (item.hasMods) {
            std::string modMsg = brls::getStr("app/library/uninstall_with_mods_prompt", displayName, item.modDetails.empty() ? "RomFS" : item.modDetails);
            brls::Dialog* modDialog = new brls::Dialog(modMsg);
            
            modDialog->addButton("app/library/uninstall_both_game_and_mods"_i18n, [this, item, displayName, currentSection, currentRow]() {
                uninstallGame(item.titleId, displayName, currentSection, currentRow, true);
            });

            modDialog->addButton("app/library/uninstall_game_only"_i18n, [this, item, displayName, currentSection, currentRow]() {
                uninstallGame(item.titleId, displayName, currentSection, currentRow, false);
            });

            modDialog->addButton("app/common/cancel"_i18n, []() {});
            modDialog->open();
        } else {
            std::string msg = brls::getStr("app/library/uninstall_confirm", displayName);
            brls::Dialog* confirmDialog = new brls::Dialog(msg);
            confirmDialog->addButton("app/common/yes"_i18n, [this, item, displayName, currentSection, currentRow]() {
                uninstallGame(item.titleId, displayName, currentSection, currentRow, false);
            });
            confirmDialog->addButton("app/common/no"_i18n, []() {});
            confirmDialog->open();
        }
    });

    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

void LibraryView::uninstallGame(uint64_t titleId, const std::string& displayName, int currentSection, int currentRow, bool deleteMods) {
    auto cancelToken = cancelToken_;
    brls::async([this, titleId, displayName, currentSection, currentRow, deleteMods, cancelToken]() {
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
            if (ok && deleteMods) {
                std::string modPath;
                for (const auto& itm : rawItems_) {
                    if (itm.titleId == titleId) {
                        modPath = itm.modPath;
                        break;
                    }
                }
                deleteGameMods(titleId, modPath);
            }

            brls::sync([this, titleId, displayName, currentSection, currentRow, ok, cancelToken]() {
                if (cancelToken && *cancelToken) return;

                if (ok) {
                    try {
                        for (auto it = rawItems_.begin(); it != rawItems_.end(); ++it) {
                            if (it->titleId == titleId) {
                                rawItems_.erase(it);
                                break;
                            }
                        }
                        rebuildSections(currentSection, currentRow);
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
}

void LibraryView::showComponentManager(const LibraryItem& item, int currentSection, int currentRow) {
    std::string displayName = item.rawName.empty() ? cleanTitle(item.game.title) : item.rawName;

    LibraryItem currentItem = item;
    for (const auto& itm : rawItems_) {
        if (itm.titleId == item.titleId) {
            currentItem = itm;
            break;
        }
    }

    auto* rootBox = new brls::Box();
    rootBox->setAxis(brls::Axis::COLUMN);
    rootBox->setWidth(680.0f);
    rootBox->setPadding(20.0f);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(brls::getStr("app/library/comp_mgr_title", displayName));
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel->setMarginBottom(4.0f);
    rootBox->addView(titleLabel);

    auto* subtitleLabel = new brls::Label();
    std::string subText = "Общий размер: " + formatBytes(currentItem.totalSizeBytes);
    if (currentItem.dlcCount > 0) {
        subText += " • DLC: " + std::to_string(currentItem.dlcCount);
    }
    if (currentItem.hasMods) {
        subText += " • Моды: " + (currentItem.modDetails.empty() ? "RomFS" : currentItem.modDetails);
    }
    subtitleLabel->setText(subText);
    subtitleLabel->setFontSize(14.0f);
    subtitleLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    subtitleLabel->setMarginBottom(16.0f);
    rootBox->addView(subtitleLabel);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setWidth(brls::View::AUTO);
    scroll->setHeight(360.0f);
    scroll->setFocusable(true);

    auto* compListBox = new brls::Box();
    compListBox->setAxis(brls::Axis::COLUMN);
    compListBox->setWidth(brls::View::AUTO);

    brls::Dialog* compDialog = new brls::Dialog(rootBox);

    auto refreshOrClose = [this, compDialog, currentItem, currentSection, currentRow]() {
        compDialog->close();
        for (const auto& itm : rawItems_) {
            if (itm.titleId == currentItem.titleId) {
                showComponentManager(itm, currentSection, currentRow);
                break;
            }
        }
    };

    if (currentItem.components.empty()) {
        auto* emptyLabel = new brls::Label();
        emptyLabel->setText("Компоненты не найдены");
        emptyLabel->setFontSize(15.0f);
        emptyLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        emptyLabel->setMarginTop(20.0f);
        compListBox->addView(emptyLabel);
    } else {
        for (size_t i = 0; i < currentItem.components.size(); ++i) {
            const auto& comp = currentItem.components[i];
            
            auto* rowBox = new brls::Box();
            rowBox->setAxis(brls::Axis::ROW);
            rowBox->setAlignItems(brls::AlignItems::CENTER);
            rowBox->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            rowBox->setWidth(brls::View::AUTO);
            rowBox->setHeight(52.0f);
            rowBox->setPaddingLeft(12.0f);
            rowBox->setPaddingRight(12.0f);
            rowBox->setCornerRadius(6.0f);
            rowBox->setMarginBottom(8.0f);
            rowBox->setBackgroundColor(nvgRGBA(128, 128, 128, 25));

            auto* leftBox = new brls::Box();
            leftBox->setAxis(brls::Axis::ROW);
            leftBox->setAlignItems(brls::AlignItems::CENTER);
            leftBox->setGrow(1.0f);

            auto* badgeBox = new brls::Box();
            badgeBox->setPadding(2.0f, 6.0f, 2.0f, 6.0f);
            badgeBox->setCornerRadius(4.0f);
            badgeBox->setMarginRight(12.0f);

            auto* badgeText = new brls::Label();
            badgeText->setFontSize(11.0f);
            badgeText->setTextColor(nvgRGB(255, 255, 255));

            if (comp.type == InstalledComponent::Type::BaseGame) {
                badgeBox->setBackgroundColor(nvgRGB(75, 85, 99)); // Slate
                badgeText->setText("BASE");
            } else if (comp.type == InstalledComponent::Type::Patch) {
                badgeBox->setBackgroundColor(nvgRGB(2, 136, 209)); // Blue
                badgeText->setText("UPD");
            } else if (comp.type == InstalledComponent::Type::Dlc) {
                badgeBox->setBackgroundColor(nvgRGB(245, 124, 0)); // Orange
                badgeText->setText("DLC");
            } else if (comp.type == InstalledComponent::Type::Mods) {
                badgeBox->setBackgroundColor(nvgRGB(124, 77, 255)); // Purple
                badgeText->setText("MODS");
            }
            badgeBox->addView(badgeText);
            leftBox->addView(badgeBox);

            auto* nameLabel = new brls::Label();
            std::string nameStr = comp.displayName;
            if (comp.sizeBytes > 0) {
                nameStr += " • " + formatBytes(comp.sizeBytes);
            }
            nameLabel->setText(nameStr);
            nameLabel->setFontSize(14.0f);
            nameLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
            leftBox->addView(nameLabel);

            rowBox->addView(leftBox);

            auto* delBtn = new brls::Button();
            delBtn->setText("app/library/comp_delete_btn"_i18n);
            delBtn->setFontSize(13.0f);
            delBtn->setWidth(120.0f);
            delBtn->setHeight(34.0f);
            delBtn->setTextColor(nvgRGB(255, 255, 255));
            delBtn->setBackgroundColor(nvgRGB(211, 47, 47));

            delBtn->registerClickAction([this, comp, currentItem, displayName, currentSection, currentRow, compDialog, refreshOrClose](brls::View* v) {
                std::string confirmMsg;
                if (comp.type == InstalledComponent::Type::BaseGame) {
                    confirmMsg = "Удаление базовой игры полностью удалит игру из памяти.\nПродолжить?";
                } else {
                    confirmMsg = brls::getStr("app/library/comp_confirm_delete", comp.displayName);
                }

                brls::Dialog* confirmDlg = new brls::Dialog(confirmMsg);
                confirmDlg->addButton("app/common/yes"_i18n, [this, comp, currentItem, displayName, currentSection, currentRow, compDialog, refreshOrClose]() {
                    if (comp.type == InstalledComponent::Type::BaseGame) {
                        compDialog->close();
                        uninstallGame(currentItem.titleId, displayName, currentSection, currentRow, false);
                    } else if (comp.type == InstalledComponent::Type::Patch) {
                        bool ok = false;
#ifdef __SWITCH__
                        NcmContentMetaKey key = {};
                        key.id = comp.titleId;
                        key.version = comp.version;
                        key.type = NcmContentMetaType_Patch;
                        key.install_type = NcmContentInstallType_Full;
                        ok = deleteNcmComponent(comp.storageId, key);
#else
                        ok = true;
#endif
                        if (ok) {
                            for (auto& itm : rawItems_) {
                                if (itm.titleId == currentItem.titleId) {
                                    itm.hasPatch = false;
                                    itm.installedPatchVersion = 0;
                                    itm.currentVersion = "1.0.0";
                                    if (itm.totalSizeBytes >= itm.patchSizeBytes) itm.totalSizeBytes -= itm.patchSizeBytes;
                                    itm.patchSizeBytes = 0;
                                    for (auto it = itm.components.begin(); it != itm.components.end(); ++it) {
                                        if (it->type == InstalledComponent::Type::Patch) {
                                            itm.components.erase(it);
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                            rebuildSections(currentSection, currentRow);
                            brls::Application::notify(brls::getStr("app/library/comp_delete_success", comp.displayName));
                            refreshOrClose();
                        } else {
                            brls::Application::notify(brls::getStr("app/library/comp_delete_failed", comp.displayName));
                        }
                    } else if (comp.type == InstalledComponent::Type::Dlc) {
                        bool ok = false;
#ifdef __SWITCH__
                        NcmContentMetaKey key = {};
                        key.id = comp.titleId;
                        key.version = comp.version;
                        key.type = NcmContentMetaType_AddOnContent;
                        key.install_type = NcmContentInstallType_Full;
                        ok = deleteNcmComponent(comp.storageId, key);
#else
                        ok = true;
#endif
                        if (ok) {
                            for (auto& itm : rawItems_) {
                                if (itm.titleId == currentItem.titleId) {
                                    if (itm.dlcCount > 0) itm.dlcCount--;
                                    if (itm.dlcSizeBytes >= comp.sizeBytes) itm.dlcSizeBytes -= comp.sizeBytes;
                                    else itm.dlcSizeBytes = 0;
                                    if (itm.totalSizeBytes >= comp.sizeBytes) itm.totalSizeBytes -= comp.sizeBytes;
                                    for (auto it = itm.components.begin(); it != itm.components.end(); ++it) {
                                        if (it->type == InstalledComponent::Type::Dlc && it->titleId == comp.titleId) {
                                            itm.components.erase(it);
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                            rebuildSections(currentSection, currentRow);
                            brls::Application::notify(brls::getStr("app/library/comp_delete_success", comp.displayName));
                            refreshOrClose();
                        } else {
                            brls::Application::notify(brls::getStr("app/library/comp_delete_failed", comp.displayName));
                        }
                    } else if (comp.type == InstalledComponent::Type::Mods) {
                        deleteGameMods(currentItem.titleId, comp.extraInfo);
                        for (auto& itm : rawItems_) {
                            if (itm.titleId == currentItem.titleId) {
                                itm.hasMods = false;
                                itm.modDetails = "";
                                if (itm.totalSizeBytes >= itm.modsSizeBytes) itm.totalSizeBytes -= itm.modsSizeBytes;
                                itm.modsSizeBytes = 0;
                                for (auto it = itm.components.begin(); it != itm.components.end(); ++it) {
                                    if (it->type == InstalledComponent::Type::Mods) {
                                        itm.components.erase(it);
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        rebuildSections(currentSection, currentRow);
                        brls::Application::notify(brls::getStr("app/library/comp_delete_mods_success", displayName));
                        refreshOrClose();
                    }
                });
                confirmDlg->addButton("app/common/no"_i18n, []() {});
                confirmDlg->open();
                return true;
            });

            rowBox->addView(delBtn);
            compListBox->addView(rowBox);
        }
    }

    scroll->setContentView(compListBox);
    rootBox->addView(scroll);

    if (currentItem.dlcCount > 1) {
        compDialog->addButton("app/library/comp_delete_all_dlc"_i18n, [this, currentItem, displayName, currentSection, currentRow, compDialog, refreshOrClose]() {
            std::string confirmMsg = brls::getStr("app/library/comp_delete_all_dlc_confirm", std::to_string(currentItem.dlcCount), displayName);
            brls::Dialog* confirmDlg = new brls::Dialog(confirmMsg);
            confirmDlg->addButton("app/common/yes"_i18n, [this, currentItem, displayName, currentSection, currentRow, compDialog, refreshOrClose]() {
#ifdef __SWITCH__
                for (const auto& comp : currentItem.components) {
                    if (comp.type == InstalledComponent::Type::Dlc) {
                        NcmContentMetaKey key = {};
                        key.id = comp.titleId;
                        key.version = comp.version;
                        key.type = NcmContentMetaType_AddOnContent;
                        key.install_type = NcmContentInstallType_Full;
                        deleteNcmComponent(comp.storageId, key);
                    }
                }
#endif
                for (auto& itm : rawItems_) {
                    if (itm.titleId == currentItem.titleId) {
                        if (itm.totalSizeBytes >= itm.dlcSizeBytes) itm.totalSizeBytes -= itm.dlcSizeBytes;
                        itm.dlcSizeBytes = 0;
                        itm.dlcCount = 0;
                        auto it = itm.components.begin();
                        while (it != itm.components.end()) {
                            if (it->type == InstalledComponent::Type::Dlc) {
                                it = itm.components.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        break;
                    }
                }
                rebuildSections(currentSection, currentRow);
                brls::Application::notify(brls::getStr("app/library/comp_delete_all_dlc_success", displayName));
                refreshOrClose();
            });
            confirmDlg->addButton("app/common/no"_i18n, []() {});
            confirmDlg->open();
        });
    }

    compDialog->addButton("app/common/cancel"_i18n, []() {});
    compDialog->open();
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
        std::string displayName = item.rawName.empty() ? cleanTitle(item.game.title) : item.rawName;
        cell->title->setText(displayName);
        
        char tidBuf[32];
        sprintf(tidBuf, "%016llX", (unsigned long long)item.titleId);
        std::string tidStrUpper(tidBuf);
        
        char tidBufLower[32];
        sprintf(tidBufLower, "%016llx", (unsigned long long)item.titleId);
        std::string tidStrLower(tidBufLower);

        std::string tidLine = "ID: " + tidStrUpper;
        if (item.totalSizeBytes > 0) {
            tidLine += " • " + formatBytes(item.totalSizeBytes);
        }
        if (!item.sdkVersion.empty()) {
            tidLine += " • SDK " + item.sdkVersion;
        }
        cell->titleId->setText(tidLine);

        // Patch badge
        if (cell->patchBadgeBox && cell->patchBadge) {
            if (item.hasPatch) {
                cell->patchBadgeBox->setVisibility(brls::Visibility::VISIBLE);
                cell->patchBadge->setText("app/library/badge_patch"_i18n);
            } else {
                cell->patchBadgeBox->setVisibility(brls::Visibility::GONE);
            }
        }

        // DLC badge
        if (cell->dlcBadgeBox && cell->dlcBadge) {
            if (item.dlcCount > 0) {
                cell->dlcBadgeBox->setVisibility(brls::Visibility::VISIBLE);
                if (item.dlcCount > 1) {
                    cell->dlcBadge->setText(brls::getStr("app/library/badge_dlc_count", std::to_string(item.dlcCount)));
                } else {
                    cell->dlcBadge->setText("app/library/badge_dlc"_i18n);
                }
            } else {
                cell->dlcBadgeBox->setVisibility(brls::Visibility::GONE);
            }
        }

        // Mod badge
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

        // Click action (A button)
        if (item.hasMods && !item.updateIgnored && item.status == GameUpdateStatus::UpdateAvailable) {
            cell->registerClickAction([parent = parent_, item](brls::View* view) {
                parent->showModWarningDialog(item);
                return true;
            });
        } else {
            cell->registerClickAction([game, rawName, tid](brls::View* view) {
                if (!game.magnet.empty()) {
                    brls::Application::pushActivity(new GameDetailView(game));
                } else {
                    std::string query;
                    if (tid != 0) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
                        query = buf;
                    } else {
                        query = getFirstTwoWords(rawName);
                        if (query.empty()) query = rawName;
                    }
                    brls::Application::pushActivity(new CatalogView(query));
                }
                return true;
            });
        }

        // Toggle update ignore action (X button)
        int currentSection = static_cast<int>(index.section);
        int currentRow = index.row;
        if (item.updateIgnored) {
            cell->registerAction("app/library/action_enable_update"_i18n, brls::ControllerButton::BUTTON_X,
                [parent = parent_, tid, displayName, currentSection, currentRow](brls::View* view) {
                    parent->toggleUpdateIgnored(tid, displayName, currentSection, currentRow);
                    return true;
                });
        } else {
            cell->registerAction("app/library/action_disable_update"_i18n, brls::ControllerButton::BUTTON_X,
                [parent = parent_, tid, displayName, currentSection, currentRow](brls::View* view) {
                    parent->toggleUpdateIgnored(tid, displayName, currentSection, currentRow);
                    return true;
                });
        }

        // Action menu (Y button)
        cell->registerAction("app/library/uninstall"_i18n, brls::ControllerButton::BUTTON_Y,
            [parent = parent_, item, currentSection, currentRow](brls::View* view) {
                parent->showGameActionMenu(item, currentSection, currentRow);
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
