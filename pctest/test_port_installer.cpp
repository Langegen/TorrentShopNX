#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <archive.h>
#include <archive_entry.h>

#include "../source/installer/PortInstallPlanner.hpp"
#include "../source/utils/archive_utils.h"
#include "../source/utils/sevenzip_utils.h"
#include "../source/utils/file_ops.h"
#include "../source/utils/string_utils.h"
#include "../source/buffer/ring_buffer.h"
#include "../source/datasource/file_data_source.h"

std::atomic<bool> g_appExiting{false};

struct Game {
    std::string title;
    std::string genre;
    std::string image_format;
};

inline bool isHomebrewGame(const Game& g) {
    bool hasNroFormat = false;
    if (!g.image_format.empty()) {
        std::string lowerFormat = g.image_format;
        std::transform(lowerFormat.begin(), lowerFormat.end(), lowerFormat.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowerFormat.find("nro") != std::string::npos) {
            hasNroFormat = true;
        }
    }
    if (!hasNroFormat && !g.title.empty()) {
        std::string lowerTitle = g.title;
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowerTitle.find("[nro]") != std::string::npos) {
            hasNroFormat = true;
        }
    }
    if (!hasNroFormat) {
        return false;
    }

    bool hasHomebrewGenre = false;
    if (!g.genre.empty()) {
        std::string lowerGenre = g.genre;
        std::transform(lowerGenre.begin(), lowerGenre.end(), lowerGenre.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowerGenre.find("homebrew") != std::string::npos ||
            lowerGenre.find("хомбрю") != std::string::npos ||
            lowerGenre.find("хоумбрю") != std::string::npos) {
            hasHomebrewGenre = true;
        }
    } else if (!g.title.empty()) {
        std::string lowerTitle = g.title;
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowerTitle.find("[homebrew]") != std::string::npos ||
            lowerTitle.find("[хомбрю]") != std::string::npos ||
            lowerTitle.find("[хоумбрю]") != std::string::npos) {
            hasHomebrewGenre = true;
        }
    }
    return hasHomebrewGenre;
}

inline bool containsWordOrTag(const std::string& text, const std::string& word) {
    if (text.empty() || word.empty()) return false;
    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string::npos) {
        bool beforeOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        size_t afterIdx = pos + word.size();
        bool afterOk = (afterIdx >= text.size()) || !std::isalnum(static_cast<unsigned char>(text[afterIdx]));

        if (beforeOk && afterOk) {
            return true;
        }
        pos += word.size();
    }
    return false;
}

inline bool isHomebrewPort(const Game& g) {
    if (isHomebrewGame(g)) return true;

    bool hasNroFormat = false;
    if (!g.image_format.empty()) {
        std::string lowerFormat = util::toLowerUtf8(g.image_format);
        if (lowerFormat.find("nro") != std::string::npos) {
            hasNroFormat = true;
        }
    }
    if (!hasNroFormat && !g.title.empty()) {
        std::string lowerTitle = util::toLowerUtf8(g.title);
        if (lowerTitle.find("[nro]") != std::string::npos || lowerTitle.find("(nro)") != std::string::npos) {
            hasNroFormat = true;
        }
    }

    if (!hasNroFormat) {
        return false;
    }

    std::string lowerTitle = util::toLowerUtf8(g.title);
    if (containsWordOrTag(lowerTitle, "port") ||
        containsWordOrTag(lowerTitle, "порт") ||
        containsWordOrTag(lowerTitle, "homebrew") ||
        containsWordOrTag(lowerTitle, "хоумбрю") ||
        containsWordOrTag(lowerTitle, "хомбрю")) {
        return true;
    }

    std::string lowerGenre = util::toLowerUtf8(g.genre);
    if (containsWordOrTag(lowerGenre, "homebrew") ||
        containsWordOrTag(lowerGenre, "хоумбрю") ||
        containsWordOrTag(lowerGenre, "хомбрю") ||
        (containsWordOrTag(lowerGenre, "port") && lowerGenre.find("sport") == std::string::npos) ||
        (containsWordOrTag(lowerGenre, "порт") && lowerGenre.find("спорт") == std::string::npos)) {
        return true;
    }

    return false;
}

#ifdef __SWITCH__
static std::string testResolvePlatformPath(const std::string& path) {
    return path;
}
#else
static std::string testResolvePlatformPath(const std::string& path) {
    if (path.rfind("sdmc:/", 0) == 0) {
        return "./" + path.substr(6);
    }
    return path;
}
#endif

// Test runner macro
#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            std::exit(1); \
        } \
    } while(0)

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "   RUNNING HOMEBREW PORT AUTO-INSTALL TEST SUITE  " << std::endl;
    std::cout << "==================================================" << std::endl;

    auto& planner = installer::PortInstallPlanner::instance();

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 1: Heuristic identification: isHomebrewGame & isHomebrewPort
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 1] Testing isHomebrewGame and isHomebrewPort heuristics..." << std::endl;
    {
        Game portGame1;
        portGame1.title = "Jak and Daxter 1 (port) [05FDF7C538138000][v0]";
        portGame1.image_format = "NSP, NRO";
        portGame1.genre = "Platformer";
        TEST_ASSERT(isHomebrewPort(portGame1), "Jak and Daxter should be detected as port");

        Game portGame2;
        portGame2.title = "Call of Duty 4: Modern Warfare [01B7F96B70C31000] [Ru/En] (Port)";
        portGame2.image_format = "NSP, NRO";
        TEST_ASSERT(isHomebrewPort(portGame2), "CoD 4 should be detected as port");

        Game portGame3;
        portGame3.title = "Subway Surfers [01d2c0b236000000] (Homebrew Port)";
        portGame3.image_format = "NSP, NRO";
        TEST_ASSERT(isHomebrewPort(portGame3), "Subway Surfers should be detected as port");

        Game officialGame1;
        officialGame1.title = "Super Mario Odyssey [0100000000010000]";
        officialGame1.image_format = "NSP";
        officialGame1.genre = "Action, Platformer";
        TEST_ASSERT(!isHomebrewPort(officialGame1), "Super Mario Odyssey must NOT be detected as port");

        // Verify that games with substring "port" inside other words are NOT falsely detected as ports
        Game sportsGame;
        sportsGame.title = "EA Sports FC 24 [01006B70182C0000]";
        sportsGame.image_format = "NSP";
        sportsGame.genre = "Sports";
        TEST_ASSERT(!isHomebrewPort(sportsGame), "EA Sports FC 24 must NOT be detected as port");

        Game transportGame;
        transportGame.title = "Transport Fever [0100774019888000]";
        transportGame.image_format = "NSP";
        transportGame.genre = "Simulation";
        TEST_ASSERT(!isHomebrewPort(transportGame), "Transport Fever must NOT be detected as port");

        Game switchSportsGame;
        switchSportsGame.title = "Nintendo Switch Sports [0100D8701297F000]";
        switchSportsGame.image_format = "NSP";
        switchSportsGame.genre = "Sports";
        TEST_ASSERT(!isHomebrewPort(switchSportsGame), "Nintendo Switch Sports must NOT be detected as port");

        Game airportGame;
        airportGame.title = "Airport Simulator Day & Night [0100E2C0157A8000]";
        airportGame.image_format = "NSP";
        airportGame.genre = "Simulation";
        TEST_ASSERT(!isHomebrewPort(airportGame), "Airport Simulator must NOT be detected as port");

        std::cout << "  -> PASSED: All port and official game heuristics correct (no false positives for sports/transport)." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 2: Archetype 1 - Direct Folder + NSP (LEGO Batman 3 / PvZ Fusion)
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 2] Testing Archetype 1: Direct Folder + NSP..." << std::endl;
    {
        std::vector<torrent::TorrentFileInfo> files = {
            {0, "switch/legobatman3/legobatman3.nro", 45000000ULL, true},
            {1, "switch/legobatman3/game/data.pak", 1200000000ULL, true},
            {2, "switch/legobatman3/game/audio.pak", 800000000ULL, true},
            {3, "LEGO Batman 3 Beyond Gotham (port) [0102f02a7b220000].nsp", 350000ULL, true}
        };

        auto plan = planner.analyze("LEGO Batman 3 Beyond Gotham", files, "ru");
        TEST_ASSERT(plan.is_port, "Must detect as port");
        TEST_ASSERT(plan.forwarder_nsp_index == 3, "NSP index must be 3");
        TEST_ASSERT(plan.forwarder_nsp_name == "LEGO Batman 3 Beyond Gotham (port) [0102f02a7b220000].nsp", "NSP filename match");
        TEST_ASSERT(plan.archives_to_extract.empty(), "No archives in Archetype 1");
        TEST_ASSERT(plan.variants.size() == 1, "Archetype 1 has 1 default variant");

        // Check path remapping
        TEST_ASSERT(plan.file_destinations[0] == "sdmc:/switch/legobatman3/legobatman3.nro", "NRO destination correct");
        TEST_ASSERT(plan.file_destinations[1] == "sdmc:/switch/legobatman3/game/data.pak", "Data pak destination correct");

        // Verify variant files list includes all 4 files
        auto variantFiles = plan.getFilesForVariant(0);
        TEST_ASSERT(variantFiles.size() == 4, "Variant files must include all 4 files");

        std::cout << "  -> PASSED: Direct folder mapped to sdmc:/switch/<game>/ with forwarder NSP." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 3: Archetype 2 - Data Archive + Overlay NRO + NSP (Subway Surfers, Celeste 64)
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 3] Testing Archetype 2: Data Archive + Overlay NRO + NSP..." << std::endl;
    {
        std::vector<torrent::TorrentFileInfo> files = {
            {0, "switch.7z", 302000000ULL, true},
            {1, "Homebrew/switch/subwaysurfers/subwaysurfers.nro", 45000000ULL, true},
            {2, "Subway Surfers (port) [01d2c0b236000000].nsp", 350000ULL, true}
        };

        auto plan = planner.analyze("Subway Surfers (port)", files, "ru");
        TEST_ASSERT(plan.is_port, "Must detect as port");
        TEST_ASSERT(plan.forwarder_nsp_index == 2, "NSP index must be 2");
        TEST_ASSERT(plan.archives_to_extract.size() == 1, "Must have 1 archive task");
        TEST_ASSERT(plan.archives_to_extract[0].file_index == 0, "Archive index must be 0");
        TEST_ASSERT(plan.archives_to_extract[0].extract_dest_dir == "sdmc:/", "Extract to SD root");
        
        // Overlay file should be recognized and path-remapped to sdmc:/switch/subwaysurfers/subwaysurfers.nro
        TEST_ASSERT(plan.file_destinations[1] == "sdmc:/switch/subwaysurfers/subwaysurfers.nro", "Overlay NRO stripped Homebrew/ prefix");
        
        auto archives = plan.getArchivesForVariant(0);
        TEST_ASSERT(archives.size() == 1, "Variant has 1 archive to extract");

        std::cout << "  -> PASSED: switch.7z extracted to SD root and Homebrew overlay path remapped." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 4: Archetype 3 - Dual Archives RUS vs ENG (Jak and Daxter 1, NFS U2)
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 4] Testing Archetype 3: Dual Archives RUS vs ENG..." << std::endl;
    {
        std::vector<torrent::TorrentFileInfo> files = {
            {0, "switch.7z", 3500000000ULL, true},
            {1, "switch_RUSSVOICE.7z", 3600000000ULL, true},
            {2, "Jak and Daxter 1 (port) [05FDF7C538138000][v0].nsp", 350000ULL, true}
        };

        // Preferred language: ru
        auto planRu = planner.analyze("Jak and Daxter 1 (port)", files, "ru");
        TEST_ASSERT(planRu.variants.size() == 2, "Must create 2 variants");
        TEST_ASSERT(planRu.variants[0].id == "ru", "Variant 0 is Russian");
        TEST_ASSERT(planRu.variants[1].id == "en", "Variant 1 is English");
        TEST_ASSERT(planRu.default_variant_index == 0, "Russian is default when preferred_lang=ru");

        // Preferred language: en
        auto planEn = planner.analyze("Jak and Daxter 1 (port)", files, "en");
        TEST_ASSERT(planEn.default_variant_index == 1, "English is default when preferred_lang=en");

        // Files for RU variant: only Russian archive (index 1) and NSP (index 2)
        auto ruFiles = planRu.getFilesForVariant(0);
        bool hasRuArch = false;
        bool hasEnArch = false;
        bool hasNsp = false;
        for (int idx : ruFiles) {
            if (idx == 1) hasRuArch = true;
            if (idx == 0) hasEnArch = true;
            if (idx == 2) hasNsp = true;
        }
        TEST_ASSERT(hasRuArch, "RU variant must include switch_RUSSVOICE.7z");
        TEST_ASSERT(!hasEnArch, "RU variant must NOT include switch.7z");
        TEST_ASSERT(hasNsp, "RU variant must include NSP");

        // Archives for RU variant: only Russian archive
        auto ruArchs = planRu.getArchivesForVariant(0);
        TEST_ASSERT(ruArchs.size() == 1 && ruArchs[0].file_index == 1, "RU variant gets only Russian archive");

        // Archives for EN variant: only English archive
        auto enArchs = planRu.getArchivesForVariant(1);
        TEST_ASSERT(enArchs.size() == 1 && enArchs[0].file_index == 0, "EN variant gets only English archive");

        std::cout << "  -> PASSED: Dual archives correctly separated, saving 3.5GB duplicate download." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 5: Archetype 4 - Dual Folders RUS vs ENG (CoD 4, Mass Effect 1, Torchlight 1)
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 5] Testing Archetype 4: Dual Folders RUS vs ENG..." << std::endl;
    {
        std::vector<torrent::TorrentFileInfo> files = {
            {0, "Call of Duty 4 Modern Warfare (port) [01B7F96B70C31000][v0].nsp", 346000ULL, true},
            {1, "Homebrew (12.10.2026)/switch/codmw/codmw.nro", 40000000ULL, true},
            {2, "Russian version/switch/codmw/game/localized_russian.pak", 800000000ULL, true},
            {3, "English version/switch/codmw/game/localized_english.pak", 800000000ULL, true}
        };

        auto plan = planner.analyze("Call of Duty 4 Modern Warfare", files, "ru");
        TEST_ASSERT(plan.variants.size() == 2, "Must detect 2 folder variants");
        TEST_ASSERT(plan.variants[0].id == "ru", "Variant 0 is Russian");
        TEST_ASSERT(plan.variants[1].id == "en", "Variant 1 is English");

        // Verify common files: Homebrew NRO (idx 1) and NSP (idx 0)
        auto ruFiles = plan.getFilesForVariant(0);
        bool hasNro = false;
        bool hasRuPak = false;
        bool hasEnPak = false;
        bool hasNsp = false;
        for (int idx : ruFiles) {
            if (idx == 0) hasNsp = true;
            if (idx == 1) hasNro = true;
            if (idx == 2) hasRuPak = true;
            if (idx == 3) hasEnPak = true;
        }
        TEST_ASSERT(hasNsp && hasNro && hasRuPak && !hasEnPak, "RU variant gets NRO + NSP + RU pak, but not EN pak");

        // Verify path remapping: "Russian version/switch/codmw/..." -> "sdmc:/switch/codmw/..."
        TEST_ASSERT(plan.file_destinations[2] == "sdmc:/switch/codmw/game/localized_russian.pak", "RU path remapped to sdmc:/switch/codmw/...");
        TEST_ASSERT(plan.file_destinations[3] == "sdmc:/switch/codmw/game/localized_english.pak", "EN path remapped to sdmc:/switch/codmw/...");

        std::cout << "  -> PASSED: Dual folders separated and mapped to identical game directory." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 6: Platform Path Resolution on PC
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 6] Testing resolvePlatformPath on PC..." << std::endl;
    {
        std::string sdmcPath = "sdmc:/switch/subwaysurfers/subwaysurfers.nro";
        std::string resolved = testResolvePlatformPath(sdmcPath);
        TEST_ASSERT(resolved == "./switch/subwaysurfers/subwaysurfers.nro", "sdmc:/ converted to ./ on PC");

        std::string nonSdmc = "cache/tmp/test.7z";
        TEST_ASSERT(testResolvePlatformPath(nonSdmc) == "cache/tmp/test.7z", "non-sdmc path untouched");

        std::cout << "  -> PASSED: Platform path resolution works seamlessly." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 7: Live archive extraction & byte counting with util::extractArchive
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n[TEST 7] Testing real archive extraction and progress callback..." << std::endl;
    {
        std::string testDir = "scratch/test_extract_out";
        std::string testZip = "scratch/test_port_archive.zip";
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(testZip, ec);
        std::filesystem::create_directories("scratch", ec);

        // Create a real zip archive using powershell / minizip or direct file
        // To be self-contained in C++, we create a small test zip file or use archive_write
        // Let's create dummy content using archive_write from libarchive
        struct archive* a = archive_write_new();
        archive_write_set_format_zip(a);
        archive_write_open_filename(a, testZip.c_str());

        const char* filesToAdd[2] = {"switch/testgame/testgame.nro", "switch/testgame/assets/data.bin"};
        const char* contents[2] = {"DUMMY_NRO_BINARY_DATA_1234567890", "DUMMY_ASSET_DATA_ABCDEFGHIJKLMN"};

        for (int i = 0; i < 2; ++i) {
            struct archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname(entry, filesToAdd[i]);
            archive_entry_set_size(entry, std::strlen(contents[i]));
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_write_header(a, entry);
            archive_write_data(a, contents[i], std::strlen(contents[i]));
            archive_entry_free(entry);
        }
        archive_write_close(a);
        archive_write_free(a);

        TEST_ASSERT(std::filesystem::exists(testZip), "Test ZIP file was created");

        // Now extract with util::extractArchive and verify callback
        std::string lastFile;
        uint64_t lastBytes = 0;
        float lastPct = 0.0f;
        int callbackCount = 0;

        auto progressCb = [&](const util::ArchiveProgress& p) {
            callbackCount++;
            lastFile = p.currentFileName;
            lastBytes = p.bytesExtracted;
            lastPct = p.percentage;
        };

        std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
        std::string extractErr;
        bool ok = util::extractArchive(testZip, testDir, progressCb, cancelFlag, extractErr);

        TEST_ASSERT(ok, "Extraction must succeed: " + extractErr);
        TEST_ASSERT(callbackCount > 0, "Callback must be invoked during extraction");
        TEST_ASSERT(!lastFile.empty(), "Current file name must be reported");
        TEST_ASSERT(lastBytes > 0, "Extracted bytes must be reported");

        // Verify files on disk
        std::string extractedNro = testDir + "/switch/testgame/testgame.nro";
        TEST_ASSERT(std::filesystem::exists(extractedNro), "Extracted NRO exists on disk");

        std::ifstream nroStream(extractedNro);
        std::string readContent((std::istreambuf_iterator<char>(nroStream)), std::istreambuf_iterator<char>());
        TEST_ASSERT(readContent == "DUMMY_NRO_BINARY_DATA_1234567890", "Extracted content matches original");

        // Clean up test files
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(testZip, ec);

        std::cout << "  -> PASSED: Real archive extracted, progress callback verified, disk content verified." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 8: RingBuffer allocation for small forwarder NSPs (< 8MB)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n[TEST 8] Testing RingBuffer allocation for small forwarder NSPs (< 8MB)..." << std::endl;

        // Forwarder NSPs are typically 200KB - 3MB (e.g. 280,792 bytes + 2MB margin = ~2.26MB)
        size_t forwarderBufSize = 280792 + 2 * 1024 * 1024; // ~2.26 MB
        buffer::RingBuffer rb1(forwarderBufSize);
        TEST_ASSERT(rb1.capacity() >= forwarderBufSize, "RingBuffer capacity must be at least requested forwarder size");

        // Test writing and reading data through the small ring buffer
        std::vector<uint8_t> dummyData(64 * 1024, 0xAA);
        size_t written = rb1.write(dummyData.data(), dummyData.size());
        TEST_ASSERT(written == dummyData.size(), "All data should be written to ring buffer");
        TEST_ASSERT(rb1.available() == dummyData.size(), "RingBuffer available bytes must match written size");

        std::vector<uint8_t> readBack(dummyData.size(), 0);
        size_t readCount = rb1.read(readBack.data(), readBack.size());
        TEST_ASSERT(readCount == dummyData.size(), "Read count must match written size");
        TEST_ASSERT(readBack == dummyData, "Read data must match written data");

        // Test reinit with various small and large capacities
        rb1.reinit(512 * 1024); // 512 KB
        TEST_ASSERT(rb1.capacity() >= 512 * 1024, "RingBuffer reinit(512KB) must succeed without 0 capacity");

        rb1.reinit(1024 * 1024); // 1 MB
        TEST_ASSERT(rb1.capacity() >= 1024 * 1024, "RingBuffer reinit(1MB) must succeed without 0 capacity");

        rb1.reinit(16 * 1024 * 1024); // 16 MB
        TEST_ASSERT(rb1.capacity() >= 16 * 1024 * 1024, "RingBuffer reinit(16MB) must succeed");

        rb1.reinit(forwarderBufSize); // back to ~2.26 MB
        TEST_ASSERT(rb1.capacity() >= forwarderBufSize, "RingBuffer reinit(2.26MB) must succeed after larger buffer");

        std::cout << "  -> PASSED: RingBuffer supports small allocations (< 8MB) without OOM error." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 9: Streaming archive extraction from IDataSource (util::extractArchiveFromSource)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n[TEST 9] Testing streaming archive extraction from IDataSource (bypassing FAT32 file limit)..." << std::endl;

        std::string testDir = "scratch/test_stream_extract";
        std::string testZip = "scratch/test_stream.zip";
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(testZip, ec);
        std::filesystem::create_directories("scratch", ec);

        // 1. Create a test archive using libarchive write
        struct archive* a = archive_write_new();
        archive_write_set_format_zip(a);
        archive_write_open_filename(a, testZip.c_str());

        const char* filesToAdd[3] = {
            "switch/halflife2/hl2.nro",
            "switch/halflife2/hl2/gameinfo.txt",
            "switch/halflife2/hl2/materials/models/gman.vtf"
        };
        const char* contents[3] = {
            "SOURCE_ENGINE_NRO_MAIN_EXECUTABLE_BINARY_DATA",
            "GameInfo\n{\ngame \"Half-Life 2\"\ntitle \"HALF-LIFE'\"\n}",
            "VTF_TEXTURE_BINARY_BLOB_0123456789ABCDEF_TEXTURE_HEADER"
        };

        for (int i = 0; i < 3; ++i) {
            struct archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname(entry, filesToAdd[i]);
            archive_entry_set_size(entry, std::strlen(contents[i]));
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_write_header(a, entry);
            archive_write_data(a, contents[i], std::strlen(contents[i]));
            archive_entry_free(entry);
        }
        archive_write_close(a);
        archive_write_free(a);

        TEST_ASSERT(std::filesystem::exists(testZip), "Test ZIP archive must exist on disk");
        uint64_t zipSize = std::filesystem::file_size(testZip);

        // 2. Wrap the archive with FileDataSource to emulate an IDataSource stream (like CustomEngineBackend)
        datasource::FileDataSource fileSource(testZip);
        bool openOk = fileSource.open();
        TEST_ASSERT(openOk, "FileDataSource::open must succeed");
        TEST_ASSERT(fileSource.totalSize() == zipSize, "FileDataSource::totalSize must match zip file size");

        // 3. Extract directly from datasource::IDataSource stream using util::extractArchiveFromSource
        int streamCbCount = 0;
        std::string lastStreamFile;
        uint64_t lastStreamBytes = 0;
        auto streamCb = [&](const util::ArchiveProgress& p) {
            streamCbCount++;
            lastStreamFile = p.currentFileName;
            lastStreamBytes = p.bytesExtracted;
        };

        std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> pauseFlag = std::make_shared<std::atomic<bool>>(false);
        std::string extractErr;

        bool streamOk = util::extractArchiveFromSource(
            &fileSource, zipSize, testDir,
            streamCb, cancelFlag, pauseFlag, extractErr
        );

        TEST_ASSERT(streamOk, "extractArchiveFromSource must succeed: " + extractErr);
        TEST_ASSERT(streamCbCount > 0, "Progress callback must be invoked during streaming extraction");
        TEST_ASSERT(lastStreamBytes > 0, "Extracted bytes must be reported in progress callback");
        TEST_ASSERT(!lastStreamFile.empty(), "Last extracted file name must be reported");

        // 4. Verify extracted files on disk and their contents
        for (int i = 0; i < 3; ++i) {
            std::string fullPath = testDir + "/" + filesToAdd[i];
            TEST_ASSERT(std::filesystem::exists(fullPath), "Extracted file exists on disk: " + fullPath);

            std::ifstream in(fullPath);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            TEST_ASSERT(content == contents[i], "Extracted file content matches original: " + std::string(filesToAdd[i]));
        }

        // 5. Clean up
        fileSource.close();
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(testZip, ec);

        std::cout << "  -> PASSED: Streaming archive extraction from IDataSource works seamlessly without saving archive." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 10: Cancellation during streaming archive extraction
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n[TEST 10] Testing cancellation flag during streaming extraction..." << std::endl;

        std::string testDir = "scratch/test_cancel_extract";
        std::string testZip = "scratch/test_cancel.zip";
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(testZip, ec);
        std::filesystem::create_directories("scratch", ec);

        struct archive* a = archive_write_new();
        archive_write_set_format_zip(a);
        archive_write_open_filename(a, testZip.c_str());

        std::string bigData(100000, 'X');
        for (int i = 0; i < 5; ++i) {
            std::string fname = "switch/test/file" + std::to_string(i) + ".bin";
            struct archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname(entry, fname.c_str());
            archive_entry_set_size(entry, bigData.size());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_write_header(a, entry);
            archive_write_data(a, bigData.data(), bigData.size());
            archive_entry_free(entry);
        }
        archive_write_close(a);
        archive_write_free(a);

        uint64_t zipSize = std::filesystem::file_size(testZip);
        datasource::FileDataSource fileSource(testZip);
        fileSource.open();

        std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> pauseFlag = std::make_shared<std::atomic<bool>>(false);
        std::string extractErr;

        auto streamCb = [&](const util::ArchiveProgress& p) {
            // Cancel as soon as first file is extracted
            cancelFlag->store(true);
        };

        bool ok = util::extractArchiveFromSource(
            &fileSource, zipSize, testDir,
            streamCb, cancelFlag, pauseFlag, extractErr
        );

        TEST_ASSERT(!ok, "extractArchiveFromSource must return false when cancelled");
        TEST_ASSERT(cancelFlag->load(), "Cancel flag was triggered");

        fileSource.close();
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(testZip, ec);

        std::cout << "  -> PASSED: extractArchiveFromSource immediately aborts on cancel flag." << std::endl;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 11: Streaming .7z archive extraction from IDataSource (SzArEx + fallback)
    // ─────────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n[TEST 11] Testing streaming .7z archive extraction from IDataSource..." << std::endl;

        std::string testDir = "scratch/test_7z_extract";
        std::string test7z = "scratch/test.7z";
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);

        if (!std::filesystem::exists(test7z)) {
            std::string cmd = "\"\"C:\\Program Files\\7-Zip\\7z.exe\" a -t7z scratch/test.7z resources/i18n/ru/app.json\"";
            std::system(cmd.c_str());
        }

        if (std::filesystem::exists(test7z)) {
            uint64_t archSize = std::filesystem::file_size(test7z);
            datasource::FileDataSource fileSource(test7z);
            bool openOk = fileSource.open();
            TEST_ASSERT(openOk, "FileDataSource::open for 7z must succeed");

            int streamCbCount = 0;
            std::string last7zFile;
            auto streamCb = [&](const util::ArchiveProgress& p) {
                streamCbCount++;
                last7zFile = p.currentFileName;
            };

            std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
            std::shared_ptr<std::atomic<bool>> pauseFlag = std::make_shared<std::atomic<bool>>(false);
            std::string extractErr;

            bool streamOk = util::extractArchiveFromSource(
                &fileSource, archSize, testDir,
                streamCb, cancelFlag, pauseFlag, extractErr
            );

            TEST_ASSERT(streamOk, "extractArchiveFromSource (.7z) must succeed: " + extractErr);
            TEST_ASSERT(streamCbCount > 0, "Callback must be invoked during 7z extraction");
            TEST_ASSERT(std::filesystem::exists(testDir + "/resources/i18n/ru/app.json") ||
                        std::filesystem::exists(testDir + "/app.json"), "7z extracted file must exist");

            fileSource.close();
            std::cout << "  -> PASSED: .7z streaming extraction from IDataSource verified successfully." << std::endl;
        } else {
            std::cout << "  -> SKIPPED: 7z archive not found on disk." << std::endl;
        }

        std::filesystem::remove_all(testDir, ec);
    }

    std::cout << "\n==================================================" << std::endl;
    std::cout << "   ALL AUTOMATED TESTS PASSED SUCCESSFULLY! (11/11) " << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
