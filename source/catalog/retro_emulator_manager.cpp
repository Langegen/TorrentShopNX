#include "retro_emulator_manager.h"
#include "../utils/app_paths.h"
#include "../utils/file_ops.h"
#include "../utils/log.h"
#include "../net/http_client.h"
#include <borealis/extern/nlohmann/json.hpp>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace catalog {

RetroEmulatorManager& RetroEmulatorManager::instance() {
    static RetroEmulatorManager inst;
    return inst;
}

std::string RetroEmulatorManager::resolvePlatformPath(const std::string& path) {
#ifdef __SWITCH__
    return path;
#else
    if (path.rfind("sdmc:/", 0) == 0) {
        return "./" + path.substr(6);
    }
    return path;
#endif
}

RetroEmulatorManager::RetroEmulatorManager() {
    initPackages();
    loadInstalledVersions();
    loadLocalManifest();
    healInstalledEmulators();
}

void RetroEmulatorManager::initPackages() {
    packages_ = {
        // --- 1. Nintendo 3DS ---
        {
            "dekopon",
            "Dekopon (Citra)",
            "PalindromicBreadLoaf",
            "2.2.3-RC1",
            "Высокопроизводительный форк Citra на базе Azahar для Nintendo Switch",
            "nintendo",
            "https://github.com/PalindromicBreadLoaf/dekopon/releases/download/v2.2.3-RC1/dekopon.nro",
            "dekopon.nro",
            "sdmc:/switch/dekopon/dekopon.nro",
            "",
            false,
            47 * 1024 * 1024,
            {"3ds"},
            nvgRGBA(244, 67, 54, 255)
        },

        // --- 2. Sony PlayStation 2 ---
        {
            "nethersx2",
            "NetherSX2",
            "NaGaa95",
            "1.2.5",
            "Нативный порт NetherSX2 для Switch с поддержкой 4 ядер CPU и SDL-интерфейсом",
            "sony",
            "https://github.com/NaGaa95/NetherSX2_nx/releases/download/1.2.5/NetherSX2.nro",
            "NetherSX2.nro",
            "sdmc:/switch/NetherSX2/NetherSX2.nro",
            "",
            false,
            98 * 1024 * 1024,
            {"ps2"},
            nvgRGBA(0, 36, 100, 255)
        },

        // --- 3. Nintendo GameCube & Wii ---
        {
            "dolphin",
            "Dolphin Standalone",
            "NaGaa95",
            "1.0.4",
            "Нативный эмулятор GameCube и Wii на базе Mesa Horizon SDK",
            "nintendo",
            "https://github.com/NaGaa95/dolphin-nx/releases/download/1.0.4/dolphin.nro",
            "dolphin.nro",
            "sdmc:/switch/dolphin/dolphin.nro",
            "",
            false,
            47 * 1024 * 1024,
            {"gamecube", "wii"},
            nvgRGBA(103, 58, 183, 255)
        },

        // --- 4. Sony PlayStation Vita ---
        {
            "vita3k",
            "Vita3K Standalone",
            "NaGaa95",
            "1.2.0",
            "Экспериментальный эмулятор PS Vita с поддержкой архивов ZIP, VPK и PKG",
            "sony",
            "https://github.com/NaGaa95/Vita3K-nx/releases/download/1.2.0/Vita3K.nro",
            "Vita3K.nro",
            "sdmc:/switch/Vita3K/Vita3K.nro",
            "",
            false,
            70 * 1024 * 1024,
            {"psvita"},
            nvgRGBA(0, 150, 214, 255)
        },

        // --- 5. Nintendo Wii U ---
        {
            "cemu",
            "Cemu Standalone",
            "NaGaa95",
            "1.1.3",
            "Нативный порт эмулятора Wii U с поддержкой OpenGL NVC0 и Zink",
            "nintendo",
            "https://github.com/NaGaa95/Cemu-nx/releases/download/1.1.3/cemu.nro",
            "cemu.nro",
            "sdmc:/switch/cemu/cemu.nro",
            "",
            false,
            82 * 1024 * 1024,
            {"wiiu"},
            nvgRGBA(0, 172, 237, 255)
        },

        // --- 6. Nintendo DS ---
        {
            "drasticds",
            "DraStic DS",
            "NaGaa95",
            "1.1.1",
            "Скоростной порт DraStic для Switch с удобным сенсорным управлением",
            "nintendo",
            "https://github.com/NaGaa95/DrasticDS_nx/releases/download/1.1.1/DrasticDS.nro",
            "DrasticDS.nro",
            "sdmc:/switch/DrasticDS/DrasticDS.nro",
            "",
            false,
            87 * 1024 * 1024,
            {"nds"},
            nvgRGBA(0, 188, 212, 255)
        },

        // --- 7. Nintendo DS (Альтернативный) ---
        {
            "melonds",
            "melonDS",
            "ArcDelta / Gheoygos",
            "7.2.1",
            "DS/DSi эмулятор с поддержкой портретного режима FlipGrip и DSi NAND",
            "nintendo",
            "https://github.com/ArcDelta/melonDS/releases/download/7.2.1/melonDS.nro",
            "melonDS.nro",
            "sdmc:/switch/melonds/melonDS.nro",
            "",
            false,
            4 * 1024 * 1024,
            {"nds"},
            nvgRGBA(0, 188, 212, 255)
        },

        // --- 8. Sony PlayStation 1 ---
        {
            "duckstation",
            "DuckStation Standalone",
            "shooterspps",
            "prerelease-2",
            "Лучший автономный эмулятор PS1 с геометрической коррекцией PGXP и апскейлом",
            "sony",
            "https://github.com/shooterspps/duckstation/releases/download/prerelease-2/duckstation_switch_prerelease_2-a7feccf-260807.zip",
            "duckstation_switch.zip",
            "sdmc:/switch/duckstation/duckstation.nro",
            "sdmc:/switch/duckstation",
            true,
            11 * 1024 * 1024,
            {"ps1"},
            nvgRGBA(0, 55, 145, 255)
        },

        // --- 9. Sony PSP ---
        {
            "ppsspp",
            "PPSSPP Community Build",
            "SirSamael",
            "0.7.0",
            "Современная сборка PPSSPP с поддержкой Vulkan (NXVK), онлайн-мультиплеера и 60 FPS",
            "sony",
            "https://github.com/SirSamael/ppsspp-switch-community-build/releases/download/v0.7.0/PPSSPP-Switch-Community-Build-v0.7.0.zip",
            "ppsspp_switch.zip",
            "sdmc:/switch/ppsspp/PPSSPP.nro",
            "sdmc:/switch/ppsspp",
            true,
            26 * 1024 * 1024,
            {"psp"},
            nvgRGBA(30, 136, 229, 255)
        },

        // --- 10. Sega Dreamcast, Naomi, Atomiswave ---
        {
            "flycast",
            "Flycast Standalone",
            "flyinghead",
            "2.7",
            "Полноскоростной эмулятор Dreamcast с поддержкой widescreen и Naomi/Atomiswave",
            "sega",
            "https://github.com/flyinghead/flycast/releases/download/v2.7/flycast-2.7.nro",
            "flycast.nro",
            "sdmc:/switch/flycast/flycast.nro",
            "",
            false,
            15 * 1024 * 1024,
            {"dreamcast"},
            nvgRGBA(255, 87, 34, 255)
        },

        // --- 11. Super Nintendo ---
        {
            "psnes",
            "pSNES",
            "Cpasjuste",
            "7.2",
            "Легковесный автономный эмулятор SNES со встроенным красивым меню и шейдерами",
            "nintendo",
            "https://github.com/Cpasjuste/pemu/releases/download/v7.2/psnes.nro",
            "psnes.nro",
            "sdmc:/switch/pSNES/psnes.nro",
            "",
            false,
            8 * 1024 * 1024,
            {"snes"},
            nvgRGBA(124, 77, 255, 255)
        },

        // --- 12. NES / Famicom ---
        {
            "pnes",
            "pNES",
            "Cpasjuste",
            "7.2",
            "Легковесный автономный эмулятор NES с постерами и быстрой загрузкой",
            "nintendo",
            "https://github.com/Cpasjuste/pemu/releases/download/v7.2/pnes.nro",
            "pnes.nro",
            "sdmc:/switch/pNES/pnes.nro",
            "",
            false,
            8 * 1024 * 1024,
            {"nes"},
            nvgRGBA(230, 0, 18, 255)
        },

        // --- 13. Sega Genesis / Mega Drive / Master System / Game Gear / Sega CD ---
        {
            "pgen",
            "pGEN",
            "Cpasjuste",
            "7.2",
            "Автономный эмулятор линейки консолей Sega с поддержкой обложек и читов",
            "sega",
            "https://github.com/Cpasjuste/pemu/releases/download/v7.2/pgen.nro",
            "pgen.nro",
            "sdmc:/switch/pGEN/pgen.nro",
            "",
            false,
            8 * 1024 * 1024,
            {"sega_md", "sega_ms", "sega_gg", "sega_cd"},
            nvgRGBA(55, 71, 79, 255)
        },

        // --- 14. Game Boy, GBC, GBA ---
        {
            "mgba",
            "mGBA Standalone",
            "endrift",
            "0.10.5",
            "Золотой стандарт эмуляции Game Boy, Color и Advance с низким инпут-лагом",
            "nintendo",
            "https://github.com/mgba-emu/mgba/releases/download/0.10.5/mGBA-0.10.5-switch.7z",
            "mGBA-0.10.5-switch.7z",
            "sdmc:/switch/mGBA/mGBA.nro",
            "sdmc:/switch/mGBA",
            true,
            16 * 1024 * 1024,
            {"gba", "gbc"},
            nvgRGBA(63, 81, 181, 255)
        },

        // --- 15. Game Boy Advance (Альтернатива pemu) ---
        {
            "pgba",
            "pGBA",
            "Cpasjuste",
            "7.2",
            "Удобный эмулятор GBA из линейки pemu с отображением бокс-артов",
            "nintendo",
            "https://github.com/Cpasjuste/pemu/releases/download/v7.2/pgba.nro",
            "pgba.nro",
            "sdmc:/switch/pGBA/pgba.nro",
            "",
            false,
            8 * 1024 * 1024,
            {"gba"},
            nvgRGBA(63, 81, 181, 255)
        },

        // --- 16. Nintendo 64 (RetroArch ядро) ---
        {
            "mupen64plus_next",
            "Mupen64Plus-Next (Core)",
            "libretro / m4xw",
            "latest",
            "Оптимизированное ядро N64 с плагином GLideN64 для запуска через RetroArch",
            "retroarch",
            "https://buildbot.libretro.com/nightly/nintendo/switch/libnx/latest/mupen64plus_next_libretro_libnx.nro",
            "mupen64plus_next_libretro_libnx.nro",
            "sdmc:/retroarch/cores/mupen64plus_next_libretro_libnx.nro",
            "",
            false,
            12 * 1024 * 1024,
            {"n64"},
            nvgRGBA(0, 160, 75, 255)
        },

        // --- 17. Sega 32X (RetroArch ядро) ---
        {
            "picodrive",
            "Picodrive 32X (Core)",
            "libretro",
            "latest",
            "Ядро для эмуляции Sega 32X с точной синхронизацией SH-2 процессоров",
            "retroarch",
            "https://buildbot.libretro.com/nightly/nintendo/switch/libnx/latest/picodrive_libretro_libnx.nro",
            "picodrive_libretro_libnx.nro",
            "sdmc:/retroarch/cores/picodrive_libretro_libnx.nro",
            "",
            false,
            4 * 1024 * 1024,
            {"sega_32x"},
            nvgRGBA(255, 152, 0, 255)
        }
    };

    // Configure bios_id and forwarder_url mappings for standard emulators
    for (auto& p : packages_) {
        if (p.id == "nethersx2") {
            p.bios_id = "ps2_bios";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/NetherSX2.forwarder.nsp";
            p.forwarder_title_id = "0510000000000002";
        } else if (p.id == "duckstation") {
            p.bios_id = "ps1_bios";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/DuckStation.forwarder.nsp";
            p.forwarder_title_id = "0510000000000003";
        } else if (p.id == "vita3k") {
            p.bios_id = "vita_fw";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/Vita3K.forwarder.nsp";
            p.forwarder_title_id = "0510000000000004";
        } else if (p.id == "flycast") {
            p.bios_id = "dreamcast_bios";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/Flycast.forwarder.nsp";
            p.forwarder_title_id = "0510000000000005";
        } else if (p.id == "pgen") {
            p.bios_id = "segacd_bios";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/pGEN.forwarder.nsp";
            p.forwarder_title_id = "0510000000000006";
        } else if (p.id == "melonds") {
            p.bios_id = "nds_bios";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/melonDS.forwarder.nsp";
            p.forwarder_title_id = "0510000000000007";
        } else if (p.id == "pnes") {
            p.bios_id = "fds_bios";
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/pNES.forwarder.nsp";
            p.forwarder_title_id = "0510000000000008";
        } else if (p.id == "picodrive") {
            p.bios_id = "sega32x_bios";
        } else if (p.id == "dekopon") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/Dekopon.forwarder.nsp";
            p.forwarder_title_id = "0510000000000001";
        } else if (p.id == "dolphin") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/Dolphin.forwarder.nsp";
            p.forwarder_title_id = "0510000000000009";
        } else if (p.id == "cemu") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/Cemu.forwarder.nsp";
            p.forwarder_title_id = "0510000000000010";
        } else if (p.id == "drasticds") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/DrasticDS.forwarder.nsp";
            p.forwarder_title_id = "0510000000000011";
        } else if (p.id == "ppsspp") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/PPSSPP.forwarder.nsp";
            p.forwarder_title_id = "0510000000000012";
        } else if (p.id == "psnes") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/pSNES.forwarder.nsp";
            p.forwarder_title_id = "0510000000000013";
        } else if (p.id == "mgba") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/mGBA.forwarder.nsp";
            p.forwarder_title_id = "0510000000000014";
        } else if (p.id == "pgba") {
            p.forwarder_url = "https://github.com/Langegen/console-games/releases/download/forwarders/pGBA.forwarder.nsp";
            p.forwarder_title_id = "0510000000000015";
        }
    }

    // --- Add Category: BIOS Packages ---
    std::vector<EmulatorPackage> biosPackages = {
        // --- 1. PlayStation 2 BIOS (NetherSX2) ---
        {
            "ps2_bios",
            "PlayStation 2 BIOS Pack",
            "Sony / Dump",
            "v2.30",
            "Комплект BIOS PS2 (v2.30 NTSC-U) для запуска игр в NetherSX2",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/pcsx2/bios/ps2-0230a-20080220.bin",
            "ps2-0230a-20080220.bin",
            "sdmc:/switch/NetherSX2/bios/ps2-0230a-20080220.bin",
            "sdmc:/switch/NetherSX2/bios",
            false,
            4 * 1024 * 1024,
            {"ps2"},
            nvgRGBA(0, 36, 100, 255)
        },

        // --- 2. PlayStation 1 BIOS (DuckStation) ---
        {
            "ps1_bios",
            "PlayStation 1 BIOS Pack",
            "Sony / Dump",
            "v5.5",
            "Дампы BIOS PS1 (SCPH-5501 US, SCPH-5502 EU, SCPH-5500 JP) для DuckStation",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/scph5501.bin",
            "scph5501.bin",
            "sdmc:/switch/duckstation/bios/scph5501.bin",
            "sdmc:/switch/duckstation/bios",
            false,
            1572864,
            {"ps1"},
            nvgRGBA(0, 55, 145, 255),
            "",
            {
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/scph5501.bin", "sdmc:/switch/duckstation/bios/scph5501.bin", 524288},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/scph5502.bin", "sdmc:/switch/duckstation/bios/scph5502.bin", 524288},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/scph5500.bin", "sdmc:/switch/duckstation/bios/scph5500.bin", 524288}
            }
        },

        // --- 3. PlayStation Vita Firmware (Vita3K) ---
        {
            "vita_fw",
            "PS Vita Firmware PUP",
            "Sony Interactive Ent.",
            "3.74",
            "Официальный пакет обновления прошивки PS Vita для первой настройки Vita3K",
            "bios",
            "http://dus01.psp2.update.playstation.net/update/psp2/image/2019_0924/sd_8b5f60b56c3da8365b973dba570c53a5/PSP2UPDAT.PUP?dest=us",
            "PSP2UPDAT.PUP",
            "sdmc:/switch/Vita3K/PSP2UPDAT.PUP",
            "sdmc:/switch/Vita3K",
            false,
            56768512,
            {"psvita"},
            nvgRGBA(0, 150, 214, 255)
        },

        // --- 4. Sega Dreamcast & Naomi (Flycast) ---
        {
            "dreamcast_bios",
            "Dreamcast BIOS & Flash",
            "Sega / Dump",
            "v1.01",
            "Системный BIOS (dc_boot.bin) и энергонезависимая память (flash.bin) для Flycast",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/dc/dc_boot.bin",
            "dc_boot.bin",
            "sdmc:/switch/flycast/data/dc_boot.bin",
            "sdmc:/switch/flycast/data",
            false,
            2228224,
            {"dreamcast"},
            nvgRGBA(255, 87, 34, 255),
            "",
            {
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/dc/dc_boot.bin", "sdmc:/switch/flycast/data/dc_boot.bin", 2097152},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/dc/flash.bin", "sdmc:/switch/flycast/data/flash.bin", 131072}
            }
        },

        // --- 5. Sega CD (pGEN) ---
        {
            "segacd_bios",
            "Sega CD BIOS Pack",
            "Sega / Dump",
            "v2.00",
            "BIOS привода Sega CD / Mega CD (US, EUR, JPN) для запуска CD-образов в pGEN",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_U.bin",
            "bios_CD_U.bin",
            "sdmc:/switch/pGEN/bios_CD_U.bin",
            "sdmc:/switch/pGEN",
            false,
            393216,
            {"sega_cd"},
            nvgRGBA(76, 175, 80, 255),
            "",
            {
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_U.bin", "sdmc:/switch/pGEN/bios_CD_U.bin", 131072},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_E.bin", "sdmc:/switch/pGEN/bios_CD_E.bin", 131072},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_J.bin", "sdmc:/switch/pGEN/bios_CD_J.bin", 131072}
            }
        },

        // --- 6. Nintendo DS (melonDS) ---
        {
            "nds_bios",
            "Nintendo DS BIOS & Firmware",
            "Nintendo / Dump",
            "v1.0",
            "Оригинальные ARM7/ARM9 BIOS и firmware.bin для melonDS (Wi-Fi, тайминги)",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios7.bin",
            "bios7.bin",
            "sdmc:/switch/melonds/bios7.bin",
            "sdmc:/switch/melonds",
            false,
            282624,
            {"nds"},
            nvgRGBA(0, 188, 212, 255),
            "",
            {
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios7.bin", "sdmc:/switch/melonds/bios7.bin", 16384},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios9.bin", "sdmc:/switch/melonds/bios9.bin", 4096},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/firmware.bin", "sdmc:/switch/melonds/firmware.bin", 262144}
            }
        },

        // --- 7. Famicom Disk System (pNES) ---
        {
            "fds_bios",
            "Famicom Disk System BIOS",
            "Nintendo / Dump",
            "v1.0",
            "Образ BIOS дисковой системы FDS (disksys.rom) для запуска .fds игр в pNES",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/disksys.rom",
            "disksys.rom",
            "sdmc:/switch/pNES/disksys.rom",
            "sdmc:/switch/pNES",
            false,
            8192,
            {"nes"},
            nvgRGBA(230, 0, 18, 255)
        },

        // --- 8. Sega 32X & Sega CD (RetroArch) ---
        {
            "sega32x_bios",
            "RetroArch Sega CD & System BIOS",
            "Sega / Libretro",
            "v2.0",
            "Набор BIOS Sega CD и системных файлов для RetroArch (cores/system)",
            "bios",
            "https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_U.bin",
            "bios_CD_U.bin",
            "sdmc:/retroarch/cores/system/bios_CD_U.bin",
            "sdmc:/retroarch/cores/system",
            false,
            393216,
            {"sega_32x", "sega_cd"},
            nvgRGBA(255, 152, 0, 255),
            "",
            {
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_U.bin", "sdmc:/retroarch/cores/system/bios_CD_U.bin", 131072},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_E.bin", "sdmc:/retroarch/cores/system/bios_CD_E.bin", 131072},
                {"https://raw.githubusercontent.com/archtaurus/RetroPieBIOS/master/BIOS/bios_CD_J.bin", "sdmc:/retroarch/cores/system/bios_CD_J.bin", 131072}
            }
        }
    };

    packages_.insert(packages_.end(), biosPackages.begin(), biosPackages.end());
}

const EmulatorPackage* RetroEmulatorManager::findPackage(const std::string& emu_id) const {
    for (const auto& p : packages_) {
        if (p.id == emu_id) return &p;
    }
    return nullptr;
}

const EmulatorPackage* RetroEmulatorManager::getPackageForConsole(const std::string& console_id) const {
    for (const auto& p : packages_) {
        for (const auto& cid : p.supported_console_ids) {
            if (cid == console_id) return &p;
        }
    }
    return nullptr;
}

bool RetroEmulatorManager::isInstalled(const std::string& emu_id) const {
    const auto* p = findPackage(emu_id);
    if (!p) return false;

    // For multi-file packages (e.g. BIOS sets with companion downloads),
    // verify that all companion files exist on disk with non-zero size
    if (!p->companion_downloads.empty()) {
        for (const auto& comp : p->companion_downloads) {
            std::string cpath = resolvePlatformPath(comp.install_path);
            struct stat cst;
            if (stat(cpath.c_str(), &cst) != 0 || cst.st_size <= 0) return false;
        }
        return true;
    }

    std::string path = resolvePlatformPath(p->install_path);
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) return true;

    // Check alternative casing and nested subfolder locations
    if (emu_id == "duckstation") {
        std::string nested = resolvePlatformPath("sdmc:/switch/duckstation/switch/duckstation/duckstation.nro");
        if (stat(nested.c_str(), &st) == 0 && st.st_size > 0) return true;
    } else if (emu_id == "ppsspp") {
        std::string alt1 = resolvePlatformPath("sdmc:/switch/ppsspp/PPSSPP.nro");
        std::string alt2 = resolvePlatformPath("sdmc:/switch/ppsspp/PPSSPP_GL.nro");
        std::string nested = resolvePlatformPath("sdmc:/switch/ppsspp/switch/ppsspp/PPSSPP.nro");
        if (stat(alt1.c_str(), &st) == 0 && st.st_size > 0) return true;
        if (stat(alt2.c_str(), &st) == 0 && st.st_size > 0) return true;
        if (stat(nested.c_str(), &st) == 0 && st.st_size > 0) return true;
    } else if (emu_id == "mgba") {
        std::string alt1 = resolvePlatformPath("sdmc:/switch/mGBA/mgba.nro");
        std::string alt2 = resolvePlatformPath("sdmc:/switch/mGBA/mGBA.nro");
        if (stat(alt1.c_str(), &st) == 0 && st.st_size > 0) return true;
        if (stat(alt2.c_str(), &st) == 0 && st.st_size > 0) return true;
    }
    return false;
}

std::string RetroEmulatorManager::getInstalledVersion(const std::string& emu_id) const {
    auto it = installed_versions_.find(emu_id);
    if (it != installed_versions_.end()) {
        return it->second;
    }
    return "";
}

EmulatorInstallStatus RetroEmulatorManager::getInstallStatus(const std::string& emu_id) const {
    if (!isInstalled(emu_id)) {
        return EmulatorInstallStatus::NOT_INSTALLED;
    }

    const auto* p = findPackage(emu_id);
    if (!p) return EmulatorInstallStatus::INSTALLED;

    std::string installedVer = getInstalledVersion(emu_id);
    if (!installedVer.empty() && installedVer != p->version && p->version != "latest") {
        return EmulatorInstallStatus::UPDATE_AVAILABLE;
    }

    return EmulatorInstallStatus::INSTALLED;
}

void RetroEmulatorManager::recordInstalledVersion(const std::string& emu_id, const std::string& version) {
    installed_versions_[emu_id] = version;
    saveInstalledVersions();
}

void RetroEmulatorManager::loadInstalledVersions() {
    installed_versions_.clear();
    std::string path = resolvePlatformPath(TSNX_DATA_DIR "/installed_emulators.txt");

    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (!k.empty() && !v.empty()) {
                installed_versions_[k] = v;
            }
        }
    }
}

void RetroEmulatorManager::saveInstalledVersions() {
    std::string path = resolvePlatformPath(TSNX_DATA_DIR "/installed_emulators.txt");
    tsnx_ensure_parent_dirs(path.c_str());

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;

    for (const auto& [k, v] : installed_versions_) {
        out << k << "=" << v << "\n";
    }
}

bool RetroEmulatorManager::uninstallEmulator(const std::string& emu_id, std::string& out_err) {
    const auto* p = findPackage(emu_id);
    if (!p) {
        out_err = "Пакет не найден в базе";
        return false;
    }

    std::error_code ec;

    // If there are companion downloads, remove each of them
    if (!p->companion_downloads.empty()) {
        for (const auto& comp : p->companion_downloads) {
            std::string cpath = resolvePlatformPath(comp.install_path);
            if (std::filesystem::exists(cpath, ec)) {
                std::filesystem::remove(cpath, ec);
            }
        }
    }

    std::string mainFile = resolvePlatformPath(p->install_path);
    if (std::filesystem::exists(mainFile, ec)) {
        std::filesystem::remove(mainFile, ec);
        if (ec) {
            out_err = "Ошибка удаления файла: " + ec.message();
            return false;
        }
    }

    // If there is an extract_dir, remove it if it exists
    if (!p->extract_dir.empty()) {
        std::string dir = resolvePlatformPath(p->extract_dir);
        if (std::filesystem::exists(dir, ec)) {
            std::filesystem::remove_all(dir, ec);
        }
    } else {
        // For standalone nro: check if parent folder is in switch/ and now empty
        std::filesystem::path fp(mainFile);
        if (fp.has_parent_path()) {
            std::filesystem::path parent = fp.parent_path();
            if (std::filesystem::is_empty(parent, ec)) {
                std::filesystem::remove(parent, ec);
            }
        }
    }

    installed_versions_.erase(emu_id);
    saveInstalledVersions();
    util::logLine("RetroEmulatorManager: uninstalled " + emu_id);
    return true;
}

const EmulatorPackage* RetroEmulatorManager::getBiosPackageForEmulator(const std::string& emu_id) const {
    const auto* emu = findPackage(emu_id);
    if (!emu || emu->bios_id.empty()) return nullptr;
    return findPackage(emu->bios_id);
}

bool RetroEmulatorManager::isForwarderInstalled(const std::string& emu_id) const {
    auto it = installed_versions_.find("forwarder_" + emu_id);
    if (it != installed_versions_.end() && it->second == "installed") {
        return true;
    }
    return false;
}

void RetroEmulatorManager::recordForwarderInstalled(const std::string& emu_id, bool installed) {
    if (installed) {
        installed_versions_["forwarder_" + emu_id] = "installed";
    } else {
        installed_versions_.erase("forwarder_" + emu_id);
    }
    saveInstalledVersions();
}

std::string RetroEmulatorManager::getManifestDownloadUrl() const {
    return "https://raw.githubusercontent.com/Langegen/console-games/main/data/emulators.json";
}

std::string RetroEmulatorManager::getLocalManifestPath() const {
    return resolvePlatformPath(std::string(TSNX_RETRO_DATA_DIR) + "/emulators.json");
}

bool RetroEmulatorManager::parseManifestFromJson(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_array()) return false;

        std::vector<EmulatorPackage> parsed;
        for (const auto& item : j) {
            EmulatorPackage p;
            p.id = item.value("id", "");
            p.name = item.value("name", "");
            p.author = item.value("author", "");
            p.version = item.value("version", "");
            p.description = item.value("description", "");
            p.category = item.value("category", "nintendo");
            p.download_url = item.value("download_url", "");
            p.filename = item.value("filename", "");
            p.install_path = item.value("install_path", "");
            p.extract_dir = item.value("extract_dir", "");
            p.is_archive = item.value("is_archive", false);
            p.file_size = item.value("file_size", (int64_t)0);

            if (item.contains("supported_console_ids") && item["supported_console_ids"].is_array()) {
                p.supported_console_ids = item["supported_console_ids"].get<std::vector<std::string>>();
            }

            if (item.contains("color") && item["color"].is_array() && item["color"].size() == 4) {
                p.color = nvgRGBA(item["color"][0].get<int>(),
                                  item["color"][1].get<int>(),
                                  item["color"][2].get<int>(),
                                  item["color"][3].get<int>());
            } else {
                p.color = nvgRGBA(128, 128, 128, 255);
            }

            p.bios_id = item.value("bios_id", "");
            p.forwarder_url = item.value("forwarder_url", "");
            p.forwarder_title_id = item.value("forwarder_title_id", "");

            if (item.contains("companion_downloads") && item["companion_downloads"].is_array()) {
                for (const auto& cd : item["companion_downloads"]) {
                    CompanionDownload comp;
                    comp.download_url = cd.value("download_url", "");
                    comp.install_path = cd.value("install_path", "");
                    comp.file_size = cd.value("file_size", (int64_t)0);
                    if (!comp.download_url.empty() && !comp.install_path.empty()) {
                        p.companion_downloads.push_back(comp);
                    }
                }
            }

            if (!p.id.empty() && !p.download_url.empty()) {
                parsed.push_back(p);
            }
        }

        if (!parsed.empty()) {
            packages_ = std::move(parsed);
            util::logLine("RetroEmulatorManager: parsed " + std::to_string(packages_.size()) + " emulators from JSON");
            return true;
        }
    } catch (const std::exception& e) {
        util::logLine(std::string("RetroEmulatorManager: parse error: ") + e.what());
    }
    return false;
}

bool RetroEmulatorManager::loadLocalManifest() {
    std::string path = getLocalManifestPath();
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string str = buffer.str();
    if (str.empty()) return false;

    return parseManifestFromJson(str);
}

bool RetroEmulatorManager::refreshManifest(std::function<void(float progress, const std::string& status)> progress_cb) {
    std::string url = getManifestDownloadUrl();
    std::string finalPath = getLocalManifestPath();
    std::string tmpPath = finalPath + ".tmp";

    tsnx_ensure_parent_dirs(tmpPath.c_str());

    util::logLine("RetroEmulatorManager: downloading manifest from " + url);
    if (progress_cb) progress_cb(0.1f, "Загрузка списка эмуляторов...");

    net::HttpClient client;
    client.setTimeout(60);

    bool dlOk = client.downloadToFile(url, tmpPath, nullptr, 60);
    if (!dlOk) {
        util::logLine("RetroEmulatorManager: failed to download manifest from " + url);
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    if (progress_cb) progress_cb(0.7f, "Разбор данных манифеста...");

    std::ifstream in(tmpPath);
    if (!in.is_open()) {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    in.close();

    bool parseOk = parseManifestFromJson(buffer.str());
    if (!parseOk) {
        util::logLine("RetroEmulatorManager: invalid JSON received from " + url);
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    // Atomic replace on disk
    std::error_code ec;
    std::filesystem::remove(finalPath, ec);
    std::filesystem::rename(tmpPath, finalPath, ec);

    if (progress_cb) progress_cb(1.0f, "Манифест успешно обновлен");
    util::logLine("RetroEmulatorManager: manifest successfully refreshed and cached at " + finalPath);
    return true;
}

void RetroEmulatorManager::healInstalledEmulators() {
    std::error_code ec;

    // 1. PPSSPP: heal nested switch/ppsspp folder
    std::string pspBase = resolvePlatformPath("sdmc:/switch/ppsspp");
    std::string pspNested = resolvePlatformPath("sdmc:/switch/ppsspp/switch/ppsspp");
    if (std::filesystem::exists(pspNested, ec)) {
        std::string moveErr;
        util::movePath(pspNested, pspBase, moveErr);
        std::filesystem::remove_all(resolvePlatformPath("sdmc:/switch/ppsspp/switch"), ec);
        util::logLine("RetroEmulatorManager: auto-healed PPSSPP folder structure (moved assets and files to " + pspBase + ")");
    } else {
        // Also check if assets alone was left behind in switch/ppsspp/assets
        std::string pspAssetsNested = resolvePlatformPath("sdmc:/switch/ppsspp/switch/ppsspp/assets");
        std::string pspAssetsTarget = resolvePlatformPath("sdmc:/switch/ppsspp/assets");
        if (std::filesystem::exists(pspAssetsNested, ec) && !std::filesystem::exists(pspAssetsTarget, ec)) {
            std::string moveErr;
            util::movePath(pspAssetsNested, pspAssetsTarget, moveErr);
            std::filesystem::remove_all(resolvePlatformPath("sdmc:/switch/ppsspp/switch"), ec);
            util::logLine("RetroEmulatorManager: auto-healed PPSSPP assets folder");
        }
    }
    std::string pspAssetsNested2 = resolvePlatformPath("sdmc:/switch/ppsspp/switch/assets");
    if (std::filesystem::exists(pspAssetsNested2, ec) && !std::filesystem::exists(resolvePlatformPath("sdmc:/switch/ppsspp/assets"), ec)) {
        std::string moveErr;
        util::movePath(pspAssetsNested2, resolvePlatformPath("sdmc:/switch/ppsspp/assets"), moveErr);
        std::filesystem::remove_all(resolvePlatformPath("sdmc:/switch/ppsspp/switch"), ec);
        util::logLine("RetroEmulatorManager: auto-healed PPSSPP switch/assets folder");
    }
    std::string pspSwitchDir = resolvePlatformPath("sdmc:/switch/ppsspp/switch");
    if (std::filesystem::exists(pspSwitchDir, ec)) {
        std::filesystem::remove_all(pspSwitchDir, ec);
    }
    std::string pspGL = resolvePlatformPath("sdmc:/switch/ppsspp/PPSSPP_GL.nro");
    std::string pspTarget = resolvePlatformPath("sdmc:/switch/ppsspp/PPSSPP.nro");
    if (std::filesystem::exists(pspGL, ec) && !std::filesystem::exists(pspTarget, ec)) {
        std::filesystem::rename(pspGL, pspTarget, ec);
    }

    // 2. DuckStation: heal nested switch/duckstation folder
    std::string duckBase = resolvePlatformPath("sdmc:/switch/duckstation");
    std::string duckNested = resolvePlatformPath("sdmc:/switch/duckstation/switch/duckstation");
    if (std::filesystem::exists(duckNested, ec)) {
        std::string moveErr;
        util::movePath(duckNested, duckBase, moveErr);
        std::filesystem::remove_all(resolvePlatformPath("sdmc:/switch/duckstation/switch"), ec);
        util::logLine("RetroEmulatorManager: auto-healed DuckStation folder structure");
    }
    std::string duckSwitchDir = resolvePlatformPath("sdmc:/switch/duckstation/switch");
    if (std::filesystem::exists(duckSwitchDir, ec)) {
        std::filesystem::remove_all(duckSwitchDir, ec);
    }

    // 3. mGBA: heal nested mGBA-*-switch folder
    std::string mgbaBase = resolvePlatformPath("sdmc:/switch/mGBA");
    if (std::filesystem::exists(mgbaBase, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(mgbaBase, ec)) {
            if (entry.is_directory()) {
                std::string dirName = entry.path().filename().string();
                std::string dirLower = dirName;
                std::transform(dirLower.begin(), dirLower.end(), dirLower.begin(), ::tolower);
                if (dirLower.find("mgba") != std::string::npos) {
                    std::string moveErr;
                    util::movePath(entry.path().string(), mgbaBase, moveErr);
                    std::filesystem::remove_all(entry.path(), ec);
                    util::logLine("RetroEmulatorManager: auto-healed mGBA folder structure");
                    break;
                }
            }
        }
        // Normalize mgba.nro -> mGBA.nro if needed
        std::string lowerNro = resolvePlatformPath("sdmc:/switch/mGBA/mgba.nro");
        std::string upperNro = resolvePlatformPath("sdmc:/switch/mGBA/mGBA.nro");
        if (std::filesystem::exists(lowerNro, ec) && !std::filesystem::exists(upperNro, ec)) {
            std::filesystem::rename(lowerNro, upperNro, ec);
        }
    }
}

} // namespace catalog
