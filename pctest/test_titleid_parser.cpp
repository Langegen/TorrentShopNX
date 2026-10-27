#include <iostream>
#include <cassert>
#include <string>
#include <iomanip>
#include "../source/utils/switch_utils.h"
#include "../source/utils/string_utils.h"

int main() {
    std::cout << "Testing Title ID and Version Parser..." << std::endl;

    // Test 1: Witcher with [Complete Edition] false-positive before real Title ID
    {
        std::string fn = "The Witcher 3: Wild Hunt [Complete Edition] [01003D100E9C6000][v0].nsz";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        uint32_t ver = util::parseVersionFromFileName(fn);
        std::cout << "Test 1: " << fn << " -> tid=0x" << std::hex << tid << " ver=" << std::dec << ver << std::endl;
        assert(tid == 0x01003D100E9C6000ULL);
        assert(ver == 0);
    }

    // Test 2: Standard bracket formatting
    {
        std::string fn = "The Witcher 3 [01003D100E9C6000][v0].nsz";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        uint32_t ver = util::parseVersionFromFileName(fn);
        std::cout << "Test 2: " << fn << " -> tid=0x" << std::hex << tid << " ver=" << std::dec << ver << std::endl;
        assert(tid == 0x01003D100E9C6000ULL);
        assert(ver == 0);
    }

    // Test 3: Parentheses formatting
    {
        std::string fn = "Mario Kart 8 Deluxe (0100152000022000) (v0).nsz";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        uint32_t ver = util::parseVersionFromFileName(fn);
        std::cout << "Test 3: " << fn << " -> tid=0x" << std::hex << tid << " ver=" << std::dec << ver << std::endl;
        assert(tid == 0x0100152000022000ULL);
        assert(ver == 0);
    }

    // Test 4: Update with large version number
    {
        std::string fn = "Mario Kart 8 Deluxe [0100152000022800][v3276800].nsz";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        uint32_t ver = util::parseVersionFromFileName(fn);
        std::cout << "Test 4: " << fn << " -> tid=0x" << std::hex << tid << " ver=" << std::dec << ver << std::endl;
        assert(tid == 0x0100152000022800ULL);
        assert(ver == 3276800);
    }

    // Test 5: Standalone filename
    {
        std::string fn = "0100152000022000.nsp";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        std::cout << "Test 5: " << fn << " -> tid=0x" << std::hex << tid << std::endl;
        assert(tid == 0x0100152000022000ULL);
    }

    // Test 6: Rejection of false positive words (length 16)
    {
        assert(util::parseTitleIdFromFileName("[Complete Edition]") == 0);
        assert(util::parseTitleIdFromFileName("[Definitive Edit]") == 0);
        assert(util::parseTitleIdFromFileName("[Collector's Edit]") == 0);
        std::cout << "Test 6: False positive words rejected OK" << std::endl;
    }

    // Test 7: Rejection of 40-char torrent hash and 32-char NCA hash
    {
        assert(util::parseTitleIdFromFileName("[134075960DE1480F5EC0B2E1C1B103DCAAD11797]") == 0);
        assert(util::parseTitleIdFromFileName("[0123456789abcdef0123456789abcdef]") == 0);
        std::cout << "Test 7: Longer hashes rejected OK" << std::endl;
    }

    // Test 8: Internal whitespace and version suffix inside bracket
    {
        std::string fn = "Mario Kart 8 Deluxe [0100152000022000 v0].nsz";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        std::cout << "Test 8: " << fn << " -> tid=0x" << std::hex << tid << std::endl;
        assert(tid == 0x0100152000022000ULL);
    }

    // Test 9: Case insensitivity (lowercase hex)
    {
        std::string fn = "game [01003d100e9c6000][v65536].nsz";
        uint64_t tid = util::parseTitleIdFromFileName(fn);
        uint32_t ver = util::parseVersionFromFileName(fn);
        std::cout << "Test 9: " << fn << " -> tid=0x" << std::hex << tid << " ver=" << std::dec << ver << std::endl;
        assert(tid == 0x01003D100E9C6000ULL);
        assert(ver == 65536);
    }

    // Test 10: Component Classification (v0 Base, Patch, DLC, Other)
    {
        auto c1 = util::classifyDownloadComponent("Super Game [0100FE701475A000][v0].nsp");
        std::cout << "Test 10a (Base A000 v0): label=" << c1.label << " name=" << c1.cleanName << std::endl;
        assert(c1.type == 0);
        assert(c1.label == "MAIN");
        assert(c1.version.empty());

        auto c2 = util::classifyDownloadComponent("Super Game [0100FE701475A800][v65536].nsp");
        std::cout << "Test 10b (Patch A800 int ver): label=" << c2.label << " name=" << c2.cleanName << std::endl;
        assert(c2.type == 1);
        assert(c2.label == "UPD");
        assert(c2.cleanName == "Патч [v65536]");
        assert(c2.version == "[v65536]");

        auto c2_sem = util::classifyDownloadComponent("Super Game [0100FE701475A800][v131072](v1.2.0).nsp");
        std::cout << "Test 10b2 (Patch A800 sem ver): label=" << c2_sem.label << " name=" << c2_sem.cleanName << std::endl;
        assert(c2_sem.type == 1);
        assert(c2_sem.label == "UPD");
        assert(c2_sem.cleanName == "Патч v1.2.0");

        auto c2_bracket = util::classifyDownloadComponent("Super Game [0100FE701475A800][1.0.3].nsp");
        std::cout << "Test 10b3 (Patch A800 bracket ver): label=" << c2_bracket.label << " name=" << c2_bracket.cleanName << std::endl;
        assert(c2_bracket.type == 1);
        assert(c2_bracket.label == "UPD");
        assert(c2_bracket.cleanName == "Патч v1.0.3");

        auto c3 = util::classifyDownloadComponent("Super Game [0100FE701475A001][v0].nsp");
        std::cout << "Test 10c (DLC A001): label=" << c3.label << " name=" << c3.cleanName << std::endl;
        assert(c3.type == 2);
        assert(c3.label == "DLC");

        auto c4 = util::classifyDownloadComponent("Game/Russian Voice Mod/atmosphere/contents/0100FE701475A000/romfs/sound.pck");
        std::cout << "Test 10d (Mod/Extra): label=" << c4.label << " name=" << c4.cleanName << std::endl;
        assert(c4.type == 3);
        assert(c4.label == "OTH");
    }

    std::cout << "\nALL 10 TESTS PASSED SUCCESSFULLY!" << std::endl;
    return 0;
}
