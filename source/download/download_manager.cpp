#include "download_manager.h"

#include <sys/stat.h>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <filesystem>

#include "../config/config.h"
#include "../datasource/custom_engine_client.h"
#include "../net/http_client.h"
#include "../utils/log.h"
#include "../utils/thread_utils.h"
#include "../utils/perf_stats.h"
#include "../utils/switch_utils.h"
#include "../engine/thread_cfg.h"
#include "../utils/app_paths.h"
#include "../catalog/retro_catalog_manager.h"
#include "../catalog/retro_layout.h"
#include "../catalog/retro_readiness.h"
#include "../utils/archive_utils.h"
#include "../utils/file_ops.h"
#include "../datasource/file_data_source.h"
#include "../datasource/concat_data_source.h"
#include "../utils/archive_peek.h"
#include "../installer/installed_ports.h"
#include "../installer/PortInstallPlanner.hpp"
#include "../GameData.hpp"

#include <engine/engine.h>

namespace ui {
    extern std::atomic<bool> g_file_select_view_active;
}

namespace {

bool customEngineGetFiles(const std::string& hash, std::vector<torrent::TorrentFileInfo>& out) {
    if (hash.empty()) return false;
    int total_files = tsnx_engine_get_file_count(nullptr, hash.c_str());
    if (total_files <= 0) total_files = 256;
    std::vector<tsnx_file_info> files(total_files);
    int n = tsnx_engine_get_files(nullptr, hash.c_str(), files.data(), total_files);
    if (n <= 0) {
        // Torrent may not be added yet; try a one-shot probe.
        std::vector<datasource::CustomEngineFileInfo> probed;
        std::string err;
        if (!datasource::CustomEngineClient::instance().probeFiles(hash, "", "", probed, &err))
            return false;
        out.clear();
        out.reserve(probed.size());
        for (const auto& f : probed) {
            torrent::TorrentFileInfo tf;
            tf.index = f.index;
            tf.name = f.path;
            tf.size = f.size;
            tf.wanted = true;
            out.push_back(std::move(tf));
        }
        return !out.empty();
    }
    out.clear();
    out.reserve(n);
    for (int i = 0; i < n; i++) {
        torrent::TorrentFileInfo tf;
        tf.index = files[i].index;
        tf.name = files[i].path;
        tf.size = files[i].size;
        tf.wanted = files[i].wanted;
        out.push_back(std::move(tf));
    }
    return true;
}

bool customEngineSetFileWanted(const std::string& hash, int file_index, bool wanted) {
    if (hash.empty()) return false;
    return tsnx_engine_set_file_wanted(nullptr, hash.c_str(), file_index, wanted);
}

void customEngineGetTorrentList(std::vector<torrent::TorrentInfo>& out) {
    out.clear();
    tsnx_torrent_item items[TSNX_MAX_TORRENTS];
    int n = tsnx_engine_get_torrents(nullptr, items, TSNX_MAX_TORRENTS);
    for (int i = 0; i < n; i++) {
        torrent::TorrentInfo ti;
        ti.id = -1;
        ti.name = items[i].name;
        ti.hash = items[i].hash;
        // Same 0..1 convention as the TorrServer path (which normalises its
        // percent_done in torrent_manager.cpp); the UI treats it as 0..1.
        ti.percent_done = items[i].progress;
        ti.download_speed_kbps = items[i].download_kbps;
        ti.loaded_size = items[i].loaded_size;
        ti.torrent_size = items[i].total_size;
        ti.seeds = items[i].seeds;
        ti.peers = items[i].peers;
        ti.known_peers = items[i].known_peers;
        ti.dht = items[i].dht_nodes;
        out.push_back(std::move(ti));
    }
}

} // namespace

namespace download {

static std::string normalizeTorrentLink(std::string link);
static std::string extractBtihHash(std::string magnet);

static bool isTransferActive(download::DownloadState state) {
    return state == download::DownloadState::Downloading ||
           state == download::DownloadState::StreamPreparing ||
           state == download::DownloadState::StreamInstalling ||
           state == download::DownloadState::Installing;
}

// Корневая папка downloads/ (куда сохраняются не-игровые файлы и Homebrew).
static std::filesystem::path downloadsBaseDir() {
    return std::filesystem::path(TSNX_DOWNLOADS_DIR);
}

// Транслитерация кириллицы (UTF-8) в латиницу для безопасной записи на FAT32/exFAT в Switch FatFs
static std::string transliterateCyrillicToAscii(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c1 = static_cast<unsigned char>(input[i]);
        if (c1 < 0x80) {
            out.push_back(static_cast<char>(c1));
            continue;
        }
        if ((c1 == 0xD0 || c1 == 0xD1) && (i + 1 < input.size())) {
            unsigned char c2 = static_cast<unsigned char>(input[++i]);
            uint16_t code = (static_cast<uint16_t>(c1) << 8) | c2;
            switch (code) {
                // Заглавные русские
                case 0xD090: out += "A"; break;
                case 0xD091: out += "B"; break;
                case 0xD092: out += "V"; break;
                case 0xD093: out += "G"; break;
                case 0xD094: out += "D"; break;
                case 0xD095: out += "E"; break;
                case 0xD081: out += "Yo"; break; // Ё
                case 0xD096: out += "Zh"; break;
                case 0xD097: out += "Z"; break;
                case 0xD098: out += "I"; break;
                case 0xD099: out += "Y"; break;
                case 0xD09A: out += "K"; break;
                case 0xD09B: out += "L"; break;
                case 0xD09C: out += "M"; break;
                case 0xD09D: out += "N"; break;
                case 0xD09E: out += "O"; break;
                case 0xD09F: out += "P"; break;
                case 0xD0A0: out += "R"; break;
                case 0xD0A1: out += "S"; break;
                case 0xD0A2: out += "T"; break;
                case 0xD0A3: out += "U"; break;
                case 0xD0A4: out += "F"; break;
                case 0xD0A5: out += "Kh"; break;
                case 0xD0A6: out += "Ts"; break;
                case 0xD0A7: out += "Ch"; break;
                case 0xD0A8: out += "Sh"; break;
                case 0xD0A9: out += "Shch"; break;
                case 0xD0AA: break; // Ъ
                case 0xD0AB: out += "Y"; break;
                case 0xD0AC: break; // Ь
                case 0xD0AD: out += "E"; break;
                case 0xD0AE: out += "Yu"; break;
                case 0xD0AF: out += "Ya"; break;

                // Строчные русские
                case 0xD0B0: out += "a"; break;
                case 0xD0B1: out += "b"; break;
                case 0xD0B2: out += "v"; break;
                case 0xD0B3: out += "g"; break;
                case 0xD0B4: out += "d"; break;
                case 0xD0B5: out += "e"; break;
                case 0xD191: out += "yo"; break; // ё
                case 0xD0B6: out += "zh"; break;
                case 0xD0B7: out += "z"; break;
                case 0xD0B8: out += "i"; break;
                case 0xD0B9: out += "y"; break;
                case 0xD0BA: out += "k"; break;
                case 0xD0BB: out += "l"; break;
                case 0xD0BC: out += "m"; break;
                case 0xD0BD: out += "n"; break;
                case 0xD0BE: out += "o"; break;
                case 0xD0BF: out += "p"; break;
                case 0xD180: out += "r"; break;
                case 0xD181: out += "s"; break;
                case 0xD182: out += "t"; break;
                case 0xD183: out += "u"; break;
                case 0xD184: out += "f"; break;
                case 0xD185: out += "kh"; break;
                case 0xD186: out += "ts"; break;
                case 0xD187: out += "ch"; break;
                case 0xD188: out += "sh"; break;
                case 0xD189: out += "shch"; break;
                case 0xD18A: break; // ъ
                case 0xD18B: out += "y"; break;
                case 0xD18C: break; // ь
                case 0xD18D: out += "e"; break;
                case 0xD18E: out += "yu"; break;
                case 0xD18F: out += "ya"; break;

                // Украинские / белорусские
                case 0xD084: out += "Ye"; break;
                case 0xD194: out += "ye"; break;
                case 0xD086: out += "I"; break;
                case 0xD196: out += "i"; break;
                case 0xD087: out += "Yi"; break;
                case 0xD197: out += "yi"; break;
                case 0xD08E: out += "U"; break;
                case 0xD19E: out += "u"; break;

                default: out += "_"; break;
            }
            continue;
        }

        // Тире и кавычки UTF-8
        if (c1 == 0xE2 && i + 2 < input.size()) {
            unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(input[i + 2]);
            if (c2 == 0x80 && (c3 == 0x93 || c3 == 0x94)) { // en-dash, em-dash
                out += "-";
                i += 2;
                continue;
            }
            if (c2 == 0x80 && (c3 == 0x98 || c3 == 0x99 || c3 == 0x9C || c3 == 0x9D)) { // quotes
                out += "_";
                i += 2;
                continue;
            }
        }

        // Прочие non-ASCII UTF-8 последовательности заменяем одним безопасным символом на кодовую точку
        if ((c1 & 0xE0) == 0xC0 && i + 1 < input.size()) {
            i += 1;
        } else if ((c1 & 0xF0) == 0xE0 && i + 2 < input.size()) {
            i += 2;
        } else if ((c1 & 0xF8) == 0xF0 && i + 3 < input.size()) {
            i += 3;
        }
        out.push_back('_');
    }
    return out;
}

// Очистка компонента пути (имени файла или папки) от недопустимых символов ФС
static std::string sanitizePathComponent(const std::string& raw) {
    std::string converted = transliterateCyrillicToAscii(raw);
    std::string out;
    out.reserve(converted.size());
    for (size_t i = 0; i < converted.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(converted[i]);
        if (c < 0x20 || c == 0x7F) continue; // control characters
        if (c == ':' || c == '/') {
            out.push_back(' ');
            out.push_back('-');
            out.push_back(' ');
        } else if (c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    // Collapse multiple consecutive spaces
    std::string collapsed;
    collapsed.reserve(out.size());
    bool in_space = false;
    for (char c : out) {
        if (c == ' ' || c == '\t') {
            if (!in_space) {
                collapsed.push_back(' ');
                in_space = true;
            }
        } else {
            collapsed.push_back(c);
            in_space = false;
        }
    }
    // Trim spaces and dots from start and end
    while (!collapsed.empty() && (collapsed.front() == ' ' || collapsed.front() == '.')) {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && (collapsed.back() == ' ' || collapsed.back() == '.')) {
        collapsed.pop_back();
    }
    if (collapsed.empty()) {
        return "download";
    }
    return collapsed;
}

// Удаление квадратных скобок с метаданными вроде "[NRO][ENG]"
static std::string stripSquareBrackets(const std::string& s) {
    return cleanTitle(s);
}

// Извлечение безопасного имени папки раздачи из названия
static std::string extractReleaseFolderName(const std::string& title) {
    std::string name = stripSquareBrackets(title);
    // Если в конце названия указано "(...)" (например имя файла из FileSelectView или тег раздачи), убираем его,
    // если перед ним остаётся непустое название игры
    size_t lastParen = name.rfind(" (");
    if (lastParen != std::string::npos && !name.empty() && name.back() == ')') {
        std::string stripped = name.substr(0, lastParen);
        while (!stripped.empty() && std::isspace(static_cast<unsigned char>(stripped.back()))) {
            stripped.pop_back();
        }
        if (!stripped.empty()) {
            name = stripped;
        }
    }
    std::string safe = sanitizePathComponent(name);
    if (safe.size() > 48) {
        safe.resize(48);
        while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.' || safe.back() == '-' || safe.back() == '_')) {
            safe.pop_back();
        }
        if (safe.empty()) safe = "download";
    }
    return safe;
}

// Относительный путь файла из торрента -> безопасный относительный путь:
// сохраняем иерархию раздачи, но очищаем сегменты от недопустимых символов.
static std::filesystem::path sanitizeTorrentPath(const std::string& raw) {
    std::string path = raw;
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    std::vector<std::string> segs;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string seg = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        pos = (next == std::string::npos) ? path.size() : next + 1;
        if (seg.empty() || seg == "." || seg == "..") continue;
        std::string safe_seg = sanitizePathComponent(seg);
        if (!safe_seg.empty() && safe_seg != "." && safe_seg != "..") {
            segs.push_back(std::move(safe_seg));
        }
    }
    std::filesystem::path out;
    for (const auto& s : segs) out /= s;
    return out;
}

static inline int fseek64(FILE* f, int64_t offset, int origin) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<__int64>(offset), origin);
#else
    return fseeko(f, static_cast<off_t>(offset), origin);
#endif
}

// Проверяет, является ли относительный путь файла модом/русификатором Atmosphere (LayeredFS).
// Если да, возвращает true и заполняет out_dest правильным целевым путём:
// на Switch: sdmc:/atmosphere/contents/<title_id>/...
// на PC: ./atmosphere/contents/<title_id>/...
static bool resolveAtmosphereModDest(const std::string& rel_path,
                                    const std::string& effective_tid,
                                    std::string& out_dest) {
    std::string norm = rel_path;
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }
    while (!norm.empty() && norm.front() == '/') norm.erase(norm.begin());

    std::string lower = norm;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    auto is16Hex = [](const std::string& s) -> bool {
        if (s.size() != 16) return false;
        for (char c : s) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    };

    // 1. Поиск "atmosphere/" в пути (например "Russian Text + Voice Mod (27.11.2024)/atmosphere/contents/0100.../romfs/...")
    size_t atmo_pos = lower.find("atmosphere/");
    if (atmo_pos != std::string::npos) {
        std::string atmo_sub = norm.substr(atmo_pos);
        std::string atmo_sub_lower = lower.substr(atmo_pos);
        // В старых модах использовался каталог atmosphere/titles/, заменяем на modern contents/
        if (atmo_sub_lower.rfind("atmosphere/titles/", 0) == 0) {
            atmo_sub = "atmosphere/contents/" + atmo_sub.substr(18);
        }
#ifdef __SWITCH__
        out_dest = "sdmc:/" + atmo_sub;
#else
        out_dest = "./" + atmo_sub;
#endif
        return true;
    }

    // 2. Поиск "contents/<16-hex-tid>/" или "titles/<16-hex-tid>/"
    for (const char* prefix : {"contents/", "titles/"}) {
        size_t p = lower.find(prefix);
        if (p != std::string::npos) {
            size_t plen = std::strlen(prefix);
            if (p + plen + 16 <= lower.size()) {
                std::string tid_cand = norm.substr(p + plen, 16);
                if (is16Hex(tid_cand)) {
                    for (char& c : tid_cand) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    std::string rest;
                    if (p + plen + 16 < norm.size() && norm[p + plen + 16] == '/') {
                        rest = norm.substr(p + plen + 17);
                    }
                    std::string atmo_path = "atmosphere/contents/" + tid_cand + (rest.empty() ? "" : ("/" + rest));
#ifdef __SWITCH__
                    out_dest = "sdmc:/" + atmo_path;
#else
                    out_dest = "./" + atmo_path;
#endif
                    return true;
                }
            }
        }
    }

    // 3. Поиск "<16-hex-tid>/romfs/", "<16-hex-tid>/exefs/" или "<16-hex-tid>/cheats/"
    for (const char* marker : {"/romfs/", "/exefs/", "/cheats/"}) {
        size_t m = lower.find(marker);
        if (m != std::string::npos && m >= 16) {
            size_t seg_start = m - 16;
            if (seg_start == 0 || norm[seg_start - 1] == '/') {
                std::string tid_cand = norm.substr(seg_start, 16);
                if (is16Hex(tid_cand)) {
                    for (char& c : tid_cand) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    std::string rest = norm.substr(m + 1); // "romfs/..."
                    std::string atmo_path = "atmosphere/contents/" + tid_cand + "/" + rest;
#ifdef __SWITCH__
                    out_dest = "sdmc:/" + atmo_path;
#else
                    out_dest = "./" + atmo_path;
#endif
                    return true;
                }
            }
        }
    }

    // 3b. Поиск сегмента 16-hex Title ID (например "0100FE701475A000/...")
    for (size_t i = 0; i + 17 <= norm.size(); ++i) {
        if ((i == 0 || norm[i - 1] == '/') && norm[i + 16] == '/') {
            std::string tid_cand = norm.substr(i, 16);
            if (is16Hex(tid_cand) && (tid_cand.rfind("0100", 0) == 0 || tid_cand.rfind("010", 0) == 0)) {
                for (char& c : tid_cand) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                std::string rest = norm.substr(i + 17);
                std::string atmo_path = "atmosphere/contents/" + tid_cand + (rest.empty() ? "" : ("/" + rest));
#ifdef __SWITCH__
                out_dest = "sdmc:/" + atmo_path;
#else
                out_dest = "./" + atmo_path;
#endif
                return true;
            }
        }
    }

    // 4. Наличие папки "romfs/", "exefs/" или "cheats/" при известном TitleID игры
    if (is16Hex(effective_tid)) {
        for (const char* marker : {"romfs/", "exefs/", "cheats/"}) {
            size_t m = lower.find(marker);
            while (m != std::string::npos) {
                if (m == 0 || norm[m - 1] == '/') {
                    std::string rest = norm.substr(m);
                    std::string tid_upper = effective_tid;
                    for (char& c : tid_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    std::string atmo_path = "atmosphere/contents/" + tid_upper + "/" + rest;
#ifdef __SWITCH__
                    out_dest = "sdmc:/" + atmo_path;
#else
                    out_dest = "./" + atmo_path;
#endif
                    return true;
                }
                m = lower.find(marker, m + 1);
            }
        }
    }

    return false;
}

// Фоновое копирование не-игровых файлов, модов/русификаторов и Homebrew/портов.
// Моды и русификаторы Nintendo Switch устанавливаются напрямую в atmosphere/contents/<title_id>/;
// остальные файлы сохраняются в downloads/<имя_раздачи>/ с контролем длины пути.
static void fileCopyWorker(datasource::IDataSource* source,
                           const std::string& hash,
                           const std::string& magnet,
                           const std::string& release_title,
                           const std::string& title_id,
                           const std::string& cover_url,
                           int file_index,
                           const std::vector<int>& selected_files,
                           bool is_homebrew,
                           const std::string& retro_console_id,
                           const std::shared_ptr<std::atomic<bool>>& cancel,
                           const std::shared_ptr<std::atomic<bool>>& pause,
                           const std::shared_ptr<FileDownloadState>& st) {
    if (!source || hash.empty()) {
        st->failed = true;
        st->error = "invalid file download request";
        return;
    }

    tsnx_engine* eng = datasource::CustomEngineClient::instance().sharedEngine();
    if (!eng) {
        st->failed = true;
        st->error = "custom engine not available";
        return;
    }

    if (!tsnx_engine_has_torrent(eng, hash.c_str())) {
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = brls::getStr("app/downloads/state_resolving_meta");
        }
        char out_hash[TSNX_MAX_HASH_LEN + 1] = {0};
        if (!tsnx_engine_add_magnet_ex(eng, magnet.c_str(), -1, true,
                                       reinterpret_cast<const volatile bool*>(cancel.get()),
                                       out_hash, sizeof(out_hash))) {
            st->failed = true;
            st->error = "failed to add torrent";
            return;
        }
    }

    int total_files = tsnx_engine_get_file_count(eng, hash.c_str());
    if (total_files <= 0) total_files = 256;
    std::vector<tsnx_file_info> files(total_files);
    int n = tsnx_engine_get_files(eng, hash.c_str(), files.data(), total_files);
    if (n <= 0) {
        st->failed = true;
        st->error = "cannot resolve torrent file info";
        return;
    }

    // Папка раздачи: для ретро-игр - папка консоли (Tico / Vita3K / Cemu), раскладку
    // файлов внутри неё задаёт catalog::retro::planLayout; для остального - downloads/<раздача>.
    std::filesystem::path release_base;
    std::string release_folder;
    const catalog::RetroConsoleInfo* retro_info = retro_console_id.empty()
        ? nullptr : catalog::RetroCatalogManager::instance().findConsole(retro_console_id);
    const catalog::retro::PathNaming retro_naming{
        [](const std::string& s) { return sanitizePathComponent(s); },
        [](const std::string& t) { return extractReleaseFolderName(t); }
    };
    if (retro_info) {
        release_base = std::filesystem::path(catalog::RetroCatalogManager::instance().getTargetDir(retro_console_id));
    } else {
        release_folder = extractReleaseFolderName(release_title);
        release_base = downloadsBaseDir() / release_folder;
    }

    // Определяем список индексов файлов для скачивания
    std::vector<int> target_indices;
    if (!selected_files.empty()) {
        target_indices = selected_files;
    } else if (file_index >= 0 && !is_homebrew) {
        target_indices.push_back(file_index);
    } else {
        // Все файлы раздачи
        target_indices.reserve(n);
        for (int i = 0; i < n; ++i) {
            target_indices.push_back(files[i].index);
        }
    }

    catalog::retro::LayoutPlan retro_plan;
    if (retro_info) {
        std::vector<catalog::retro::TorrentFileRef> refs;
        for (int idx : target_indices) {
            if (idx >= 0 && idx < n) {
                refs.push_back({idx, files[idx].path, static_cast<uint64_t>(files[idx].size)});
            }
        }
        retro_plan = catalog::retro::planLayout(*retro_info, release_title, refs, retro_naming);
        if (!retro_plan.indices.empty()) target_indices = retro_plan.indices;
    }

    if (target_indices.empty()) {
        st->failed = true;
        st->error = "no target files to download";
        return;
    }

    auto resolveDestRel = [&](const std::string& rel_path) -> std::filesystem::path {
        if (retro_info) {
            auto it = retro_plan.dest_rel.find(rel_path);
            if (it != retro_plan.dest_rel.end()) return std::filesystem::path(it->second);
            return sanitizeTorrentPath(rel_path);
        }
        std::filesystem::path p = sanitizeTorrentPath(rel_path);
        if (!release_folder.empty()) {
            auto it = p.begin();
            if (it != p.end() && std::next(it) != p.end()) {
                std::string firstSeg = it->string();
                if (firstSeg.size() == release_folder.size() &&
                    std::equal(firstSeg.begin(), firstSeg.end(), release_folder.begin(),
                               [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); })) {
                    std::filesystem::path stripped;
                    for (++it; it != p.end(); ++it) {
                        stripped /= *it;
                    }
                    if (!stripped.empty()) {
                        p = stripped;
                    }
                }
            }
        }
        return p;
    };

    // Ретро-игры: сначала ставим то, чем игра запускается (Tico и его модуль,
    // Vita3K или Cemu), если этого ещё нет. Без сети игра всё равно скачается.
    if (retro_info) {
        std::string install_err;
        bool installed = catalog::ensureRunnerInstalled(retro_console_id,
            [&](const std::string& package_name, const catalog::InstallProgress& p) {
                std::lock_guard<std::mutex> lk(st->msg_mutex);
                st->stage_label = brls::getStr("app/retro/stage_install_runner", package_name,
                                               std::to_string(static_cast<int>(p.fraction * 100.0f)));
            },
            cancel, install_err);
        if (!installed) {
            util::logLine("download: emulator for " + retro_console_id + " was not installed: " + install_err);
        }
        std::lock_guard<std::mutex> lk(st->msg_mutex);
        st->stage_label.clear();
    }

    uint64_t total_size = 0;
    for (int idx : target_indices) {
        if (idx >= 0 && idx < n) {
            total_size += static_cast<uint64_t>(files[idx].size);
        }
    }
    st->size = total_size;
    st->total.store(total_size);

    uint64_t cumulative_written = 0;
    std::string first_dest;
    bool all_ok = true;

    std::string effective_tid = title_id;
    if (effective_tid.empty()) {
        for (int i = 0; i < n; ++i) {
            uint64_t tid = util::parseTitleIdFromFileName(files[i].path);
            if (tid != 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
                effective_tid = buf;
                break;
            }
        }
    }
    if (effective_tid.empty()) {
        uint64_t tid = util::parseTitleIdFromFileName(release_title);
        if (tid != 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
            effective_tid = buf;
        }
    }

    bool any_mod = false;
    std::string mod_base_dir;

    for (int idx : target_indices) {
        if (cancel->load() || g_appExiting.load()) {
            all_ok = false;
            break;
        }
        while (pause && pause->load() && !cancel->load() && !g_appExiting.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (cancel->load() || g_appExiting.load()) {
            all_ok = false;
            break;
        }
        if (idx < 0 || idx >= n) continue;

        uint64_t file_size = static_cast<uint64_t>(files[idx].size);
        std::string rel_path = files[idx].path;
        std::filesystem::path dest_rel = resolveDestRel(rel_path);

        bool is_archive = util::isArchiveFile(rel_path);
        bool is_mod = false;
        std::string mod_dest;
        if (retro_console_id.empty() && !is_homebrew && !is_archive) {
            is_mod = resolveAtmosphereModDest(rel_path, effective_tid, mod_dest);
        }

        std::string file_dest;
        if (is_mod) {
            file_dest = mod_dest;
            any_mod = true;
            if (mod_base_dir.empty()) {
                std::string tid_val = effective_tid.empty() ? "0100000000000000" : effective_tid;
#ifdef __SWITCH__
                mod_base_dir = "sdmc:/atmosphere/contents/" + tid_val;
#else
                mod_base_dir = "./atmosphere/contents/" + tid_val;
#endif
            }

            // Миграция ранее частично/полностью скачанных файлов из downloads/ в atmosphere/contents/
            std::filesystem::path old_loc = release_base / dest_rel;
            std::error_code ec_old;
            if (std::filesystem::exists(old_loc, ec_old) && !std::filesystem::is_directory(old_loc, ec_old)) {
                uint64_t old_sz = std::filesystem::file_size(old_loc, ec_old);
                if (old_sz > 0 && !std::filesystem::exists(file_dest, ec_old)) {
                    tsnx_ensure_parent_dirs(file_dest.c_str());
                    std::error_code ec_mv;
                    std::filesystem::rename(old_loc, file_dest, ec_mv);
                    if (ec_mv) {
                        std::filesystem::copy_file(old_loc, file_dest, std::filesystem::copy_options::overwrite_existing, ec_mv);
                        if (!ec_mv) std::filesystem::remove(old_loc, ec_mv);
                    }
                    if (!ec_mv) {
                        util::logLine("download: migrated existing mod file from " + old_loc.string() + " -> " + file_dest + " (" + std::to_string(old_sz) + " bytes)");
                    }
                }
            }
        } else {
            file_dest = (release_base / dest_rel).string();
            // Предотвращение ошибки FAT32 EIO при длине пути > 240 символов
            if (file_dest.size() > 240) {
                std::string fname = dest_rel.filename().string();
                std::string base_s = release_base.string();
                size_t max_parent_len = (240 > (base_s.size() + fname.size() + 2))
                    ? (240 - base_s.size() - fname.size() - 2) : 10;
                std::string parent_s = dest_rel.parent_path().generic_string();
                if (parent_s.size() > max_parent_len) {
                    parent_s.resize(max_parent_len);
                    while (!parent_s.empty() && (parent_s.back() == '/' || parent_s.back() == ' ')) parent_s.pop_back();
                }
                file_dest = (release_base / std::filesystem::path(parent_s) / fname).string();
            }
        }
        if (first_dest.empty()) first_dest = file_dest;

        if (file_size == 0) {
            tsnx_ensure_parent_dirs(file_dest.c_str());
            FILE* empty_f = std::fopen(file_dest.c_str(), "wb");
            if (empty_f) std::fclose(empty_f);
            continue;
        }

        // Проверка: файл уже полностью скачан
        std::error_code ec_exist;
        uint64_t existing_size = std::filesystem::exists(file_dest, ec_exist) ? std::filesystem::file_size(file_dest, ec_exist) : 0;
        if (existing_size >= file_size) {
            cumulative_written += file_size;
            st->written.store(cumulative_written);
            source->notifyStreamingComplete(true);
            util::logLine("download: file already fully downloaded: " + file_dest);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = brls::getStr("app/downloads/state_connecting_peers");
        }
        source->setSchedulerEnabled(true);
        source->setCancelFlag(cancel.get());
        if (cancel->load() || !source->open(hash, idx)) {
            st->failed = true;
            st->error = "failed to open stream for " + rel_path;
            all_ok = false;
            break;
        }

        tsnx_ensure_parent_dirs(file_dest.c_str());

        // Horizon OS micro-SD cluster alignment: 2 MB write chunks with dedicated stream buffer
        static constexpr size_t kChunk = 2 * 1024 * 1024;
        uint64_t offset = 0;
        FILE* f = nullptr;

        if (existing_size > 0 && existing_size < file_size) {
            uint64_t resume_offset = (existing_size / kChunk) * kChunk;
            f = std::fopen(file_dest.c_str(), "r+b");
            if (f && fseek64(f, static_cast<int64_t>(resume_offset), SEEK_SET) == 0) {
                offset = resume_offset;
                cumulative_written += resume_offset;
                st->written.store(cumulative_written);
                util::logLine("download: resuming partial file at offset=" + std::to_string(offset) + " / " + std::to_string(file_size));
            } else {
                if (f) std::fclose(f);
                f = nullptr;
                offset = 0;
            }
        }
        if (!f) {
            f = std::fopen(file_dest.c_str(), "wb");
        }
        if (!f) {
            st->failed = true;
            st->error = "cannot create destination file: " + file_dest +
                        (errno != 0 ? (" (" + std::string(strerror(errno)) + ")") : "");
            util::logLine("download: " + st->error);
            source->notifyStreamingComplete(false);
            all_ok = false;
            break;
        }

        std::vector<unsigned char> fbuf(kChunk);
        std::setvbuf(f, reinterpret_cast<char*>(fbuf.data()), _IOFBF, kChunk);

        std::vector<unsigned char> buf(kChunk);
        bool file_ok = true;

        while (offset < file_size && !cancel->load() && !g_appExiting.load()) {
            if (pause && pause->load()) {
                std::fflush(f);
                while (pause->load() && !cancel->load() && !g_appExiting.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (cancel->load() || g_appExiting.load()) break;
            }

            size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, file_size - offset));
            size_t got = source->read(offset, buf.data(), want);
            if (got == 0) {
                if (cancel->load() || g_appExiting.load()) break;
                if (pause && pause->load()) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(st->msg_mutex);
                if (!st->stage_label.empty()) st->stage_label.clear();
            }
            if (std::fwrite(buf.data(), 1, got, f) != got) {
                file_ok = false;
                st->error = "disk write failed";
                break;
            }
            offset += got;
            cumulative_written += got;
            st->written.store(cumulative_written);
        }
        std::fclose(f);

        if (file_ok && offset >= file_size) {
            source->notifyStreamingComplete(true);
        } else {
            all_ok = false;
            if (cancel->load() || g_appExiting.load()) {
                st->failed = true;
                st->error = "download cancelled";
                std::error_code e2;
                std::filesystem::remove(file_dest, e2);
            } else if (!file_ok) {
                st->failed = true;
                if (st->error.empty()) st->error = "disk write failed";
                std::error_code e2;
                std::filesystem::remove(file_dest, e2);
            } else {
                st->failed = true;
                if (st->error.empty()) st->error = "download failed";
            }
            source->notifyStreamingComplete(false);
            break;
        }
    }

    if (all_ok && !cancel->load()) {
        // 1. Автораспаковка Switch модов/русификаторов из архивов (.zip, .7z, .rar)
        if (retro_console_id.empty() && !is_homebrew) {
            for (int idx : target_indices) {
                if (cancel->load() || g_appExiting.load()) break;
                if (idx < 0 || idx >= n) continue;
                std::string rel_path = files[idx].path;
                std::filesystem::path dest_rel = resolveDestRel(rel_path);
                std::string file_dest = (release_base / dest_rel).string();

                if (util::isArchiveFile(file_dest)) {
                    std::string archive_tid = effective_tid;
                    bool is_mod_arch = false;

                    // Сканируем заголовки архива на наличие LayeredFS файлов
                    util::iterateArchiveEntries(file_dest, [&](const std::string& entryName, bool isDir) -> bool {
                        (void)isDir;
                        std::string test_dest;
                        if (resolveAtmosphereModDest(entryName, archive_tid, test_dest)) {
                            is_mod_arch = true;
                            if (archive_tid.empty()) {
                                size_t p = test_dest.find("contents/");
                                if (p != std::string::npos && p + 9 + 16 <= test_dest.size()) {
                                    archive_tid = test_dest.substr(p + 9, 16);
                                }
                            }
                            return false; // Нашли мод-файл, прекращаем сканирование
                        }
                        return true;
                    });

                    if (is_mod_arch) {
                        if (archive_tid.empty()) {
                            uint64_t tid = util::parseTitleIdFromFileName(file_dest);
                            if (tid == 0) tid = util::parseTitleIdFromFileName(release_title);
                            if (tid != 0) {
                                char buf[32];
                                std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
                                archive_tid = buf;
                            }
                        }

                        if (!archive_tid.empty()) {
                            {
                                std::lock_guard<std::mutex> lk(st->msg_mutex);
                                st->stage_label = brls::getStr("app/downloads/state_installing");
                            }

                            auto modPathTransform = [&](const std::string& entryName, bool isDir) -> std::string {
                                (void)isDir;
                                std::string dest;
                                if (resolveAtmosphereModDest(entryName, archive_tid, dest)) {
                                    return dest;
                                }
                                return ""; // Пропускаем файлы, не относящиеся к моду
                            };

                            std::string outErr;
                            util::logLine("download: auto-extracting Switch mod archive: " + file_dest + " -> atmosphere/contents/" + archive_tid);
                            bool extractOk = util::extractArchive(file_dest, "", nullptr, cancel, outErr, modPathTransform);
                            if (extractOk) {
                                util::logLine("download: Switch mod auto-extract succeeded for " + file_dest);
                                std::error_code ec_rm;
                                std::filesystem::remove(file_dest, ec_rm);
                                any_mod = true;
#ifdef __SWITCH__
                                mod_base_dir = "sdmc:/atmosphere/contents/" + archive_tid;
#else
                                mod_base_dir = "./atmosphere/contents/" + archive_tid;
#endif
                            } else {
                                util::logLine("download: Switch mod auto-extract failed: " + outErr);
                            }
                        } else {
                            util::logLine("download: Switch mod archive detected, but TitleID is unknown: " + file_dest);
                        }
                    }
                }
            }
        }

        // 2. Ретро-игры: распаковка, плейлисты .m3u, исправление .cue, обложка для Tico.
        if (retro_info && !cancel->load() && !g_appExiting.load()) {
            catalog::retro::FinalizeCallbacks retro_cb;
            retro_cb.on_stage = [&](const std::string& key) {
                std::lock_guard<std::mutex> lk(st->msg_mutex);
                st->stage_label = brls::getStr(key);
            };
            retro_cb.on_extract = [&](const std::string& archive_name, int percent) {
                std::lock_guard<std::mutex> lk(st->msg_mutex);
                st->stage_label = brls::getStr("app/retro/stage_extracting", archive_name, std::to_string(percent));
            };
            catalog::retro::FinalizeResult fin =
                catalog::retro::finalizeRelease(*retro_info, retro_plan, retro_naming, retro_cb, cancel);

            // One game per release gets the release cover, named after what Tico lists.
            if (retro_info->route == catalog::RetroRoute::Tico && fin.launch_entries.size() == 1 &&
                !cover_url.empty() && !cancel->load()) {
                retro_cb.on_stage("app/retro/stage_cover");
                catalog::retro::saveTicoCover(retro_info->tico_slug,
                                              std::filesystem::path(fin.launch_entries.front()).stem().string(),
                                              cover_url);
            }

            catalog::RetroReadiness readiness = catalog::checkRetroReadiness(retro_console_id);
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            if (fin.unsupported_format) {
                st->result_key = "app/retro/ready_check_format";
            } else if (!readiness.emulatorReady()) {
                st->result_key = "app/retro/ready_need_emulator";
            } else if (!readiness.systemFilesReady()) {
                st->result_key = (retro_info->route == catalog::RetroRoute::Vita3K)
                    ? "app/retro/ready_need_vita_fw" : "app/retro/ready_need_bios";
            } else {
                switch (retro_info->route) {
                    case catalog::RetroRoute::Tico:
                        st->result_key = "app/retro/ready_in_tico";
                        break;
                    case catalog::RetroRoute::Vita3K:
                        st->result_key = "app/retro/ready_vita3k";
                        break;
                    case catalog::RetroRoute::Cemu:
                        st->result_key = "app/retro/ready_cemu";
                        break;
                }
            }
            st->stage_label = brls::getStr(st->result_key);
        }

        if (any_mod && !mod_base_dir.empty()) {
            st->dest = mod_base_dir;
            util::logLine("download: mod/rusifier installed to: " + mod_base_dir);
        } else {
            st->dest = release_base.string();
        }
        st->done = true;
        st->written.store(total_size);

        if (any_mod) {
            // Очищаем пустую старую директорию в downloads/ если файлы были мигрированы или архивы распакованы
            std::filesystem::path old_mod_dir = downloadsBaseDir() / release_folder;
            std::error_code ec_rm;
            try {
                if (std::filesystem::exists(old_mod_dir, ec_rm)) {
                    bool has_files = false;
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(old_mod_dir, std::filesystem::directory_options::skip_permission_denied, ec_rm)) {
                        if (entry.is_regular_file(ec_rm)) {
                            has_files = true;
                            break;
                        }
                    }
                    if (!has_files && !ec_rm) {
                        std::filesystem::remove_all(old_mod_dir, ec_rm);
                        util::logLine("download: cleaned up old empty mod directory in downloads: " + old_mod_dir.string());
                    }
                }
            } catch (...) {}
        }
    } else {
        st->failed = true;
        if (st->error.empty()) st->error = "download cancelled";
    }
}

#ifdef __SWITCH__
static std::string resolvePlatformPath(const std::string& path) {
    return path;
}
#else
static std::string resolvePlatformPath(const std::string& path) {
    if (path.rfind("sdmc:/", 0) == 0) {
        return "./" + path.substr(6);
    }
    return path;
}
#endif


// Фоновый воркер автоматической установки портов хоумбрю:
// 1. Скачивание архивов раздачи во временную папку cache/tmp/
// 2. Распаковка архивов на карту памяти (sdmc:/) с отображением живого прогресса каждого файла
// 3. Удаление временных архивов для экономии места на карте памяти
// 4. Скачивание loose/overlay файлов (обновлений .nro, русификаторов) в sdmc:/switch/<game>/
// 5. Скачивание и установка forwarder NSP через HybridNspInstaller в ОС Switch
static void portInstallWorker(datasource::IDataSource* source,
                              const std::string& hash,
                              const std::string& magnet,
                              const std::string& release_title,
                              const std::string& topic_id,
                              std::shared_ptr<installer::PortInstallPlan> plan,
                              int variant_index,
                              bool with_optional,
                              const std::shared_ptr<std::atomic<bool>>& cancel,
                              const std::shared_ptr<std::atomic<bool>>& pause,
                              const std::shared_ptr<PortInstallProgressState>& st,
                              std::shared_ptr<PortInstallResume> resume) {
    if (!source || hash.empty()) {
        st->failed = true;
        st->error = "app/ports/error_engine"_i18n;
        return;
    }

    tsnx_engine* eng = datasource::CustomEngineClient::instance().sharedEngine();
    if (!eng) {
        st->failed = true;
        st->error = "app/ports/error_engine"_i18n;
        return;
    }

    if (!tsnx_engine_has_torrent(eng, hash.c_str())) {
        char out_hash[TSNX_MAX_HASH_LEN + 1] = {0};
        if (!tsnx_engine_add_magnet_ex(eng, magnet.c_str(), -1, true,
                                       reinterpret_cast<const volatile bool*>(cancel.get()),
                                       out_hash, sizeof(out_hash))) {
            st->failed = true;
            st->error = "app/detail/auto_install_probe_failed"_i18n;
            return;
        }
    }

    int total_files = tsnx_engine_get_file_count(eng, hash.c_str());
    if (total_files <= 0) {
        st->failed = true;
        st->error = "app/detail/auto_install_probe_failed"_i18n;
        return;
    }
    std::vector<tsnx_file_info> files(total_files);
    int n = tsnx_engine_get_files(eng, hash.c_str(), files.data(), total_files);
    if (n <= 0) {
        st->failed = true;
        st->error = "app/detail/auto_install_probe_failed"_i18n;
        return;
    }
    files.resize(static_cast<size_t>(n));
    // Below files[idx] is addressed by the torrent file index: keep them in that order.
    std::sort(files.begin(), files.end(), [](const tsnx_file_info& a, const tsnx_file_info& b) {
        return a.index < b.index;
    });
    for (int i = 0; i < n; ++i) {
        if (files[i].index != i) {
            st->failed = true;
            st->error = "app/detail/auto_install_probe_failed"_i18n;
            util::logLine("port_installer: file #" + std::to_string(i) + " has index " + std::to_string(files[i].index));
            return;
        }
    }
    if (!resume) resume = std::make_shared<PortInstallResume>();

    std::vector<torrent::TorrentFileInfo> tfiles(n);
    for (int i = 0; i < n; ++i) {
        tfiles[i].index = files[i].index;
        tfiles[i].name = files[i].path;
        tfiles[i].size = files[i].size;
        tfiles[i].wanted = true;
    }
    // The file list in the format of pctest/port_fixtures (grep port_file | cut -f2-).
    for (int i = 0; i < n && i < 2000; ++i) {
        util::logLine("port_file\t" + std::to_string(files[i].size) + "\t" + files[i].path);
    }
    if (n > 2000) util::logLine("port_file: " + std::to_string(n - 2000) + " more files not logged");

    if (!plan) {
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = "app/ports/stage_analyzing"_i18n;
        }
        std::string userLang = config::ConfigManager::instance().getLanguage();
        if (userLang.empty() || userLang == "auto") userLang = brls::Application::getLocale();   // "auto": UI language
        auto analyzed = std::make_shared<installer::PortInstallPlan>(
            installer::PortInstallPlanner::instance().plan(topic_id, release_title, tfiles, userLang));
        if (!analyzed->is_port) {
            st->failed = true;
            st->error = "app/detail/auto_install_not_port"_i18n;
            return;
        }
        plan = analyzed;
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->resolved_plan = plan;
        }
    }

    // -1: the user did not pick a version, use the one for the UI language.
    if (variant_index < 0 || variant_index >= static_cast<int>(plan->variants.size())) {
        variant_index = plan->default_variant_index;
        if (variant_index < 0 || variant_index >= static_cast<int>(plan->variants.size())) variant_index = 0;
    }
    st->resolved_variant.store(variant_index);

    // Held for the whole install: a rollback after deleting the item waits for us.
    std::unique_lock<std::mutex> resume_lock(resume->mtx);
    if (resume->variant != variant_index) {
        // Another version was picked: earlier stages do not count.
        resume->done_archives.clear();
        resume->done_files.clear();
        resume->done_forwarders.clear();
        resume->variant = variant_index;
    } else if (!resume->done_archives.empty() || !resume->done_files.empty()) {
        util::logLine("port_installer: retry, already done: " + std::to_string(resume->done_archives.size()) +
                      " archives, " + std::to_string(resume->done_files.size()) + " files, " +
                      std::to_string(resume->done_forwarders.size()) + " forwarders");
    }
    // Cancel: remove everything this install created. Failure: keep it for "Retry".
    struct ResumeFinalizer {
        PortInstallResume& r;
        const std::shared_ptr<std::atomic<bool>>& cancel;
        ~ResumeFinalizer() {
            if (cancel->load()) {
                util::logLine("port_installer: cancelled, removing " + std::to_string(r.journal.fileCount()) +
                              " created files");
                r.journal.rollback();
                r.written.clear();
                r.done_archives.clear();
                r.done_files.clear();
                r.done_forwarders.clear();
                r.variant = -1;
            }
        }
    } resume_finalizer{*resume, cancel};
    util::logLine("port_installer: variant " + std::to_string(variant_index) +
                  (plan->variants.empty() ? std::string() : " (" + plan->variants[variant_index].id + ")"));
    {
        // Full plan, one line per file (multi-line message is split for the log).
        std::string desc = plan->describe(tfiles);
        size_t pos = 0;
        while (pos < desc.size()) {
            size_t nl = desc.find('\n', pos);
            if (nl == std::string::npos) nl = desc.size();
            if (nl > pos) util::logLine(desc.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }

    std::vector<int> target_indices = plan->getFilesForVariant(variant_index, with_optional);
    if (with_optional) util::logLine("port_installer: with optional extras (" + std::to_string(plan->optional_file_indices.size()) + " files)");
    if (target_indices.empty()) {
        st->failed = true;
        st->error = "app/ports/error_no_files"_i18n;
        return;
    }

    // Refuse before downloading anything if an archive cannot be streamed.
    auto archives = plan->getArchivesForVariant(variant_index, with_optional);
    for (const auto& a : archives) {
        if (!a.unsupported_reason.empty()) {
            st->failed = true;
            st->error = brls::getStr("app/ports/error_unsupported_archive", std::filesystem::path(a.archive_torrent_path).filename().string());
            util::logLine("port_installer: " + st->error);
            return;
        }
    }

    uint64_t total_dl_bytes = 0;
    for (int idx : target_indices) {
        if (idx >= 0 && idx < n) {
            total_dl_bytes += static_cast<uint64_t>(files[idx].size);
        }
    }
    st->dl_total.store(total_dl_bytes);
    st->dl_written.store(0);
    st->dl_progress.store(0.0f);

    // Настраиваем приоритеты в движке
    for (int i = 0; i < n; ++i) {
        bool wanted = (std::find(target_indices.begin(), target_indices.end(), files[i].index) != target_indices.end());
        tsnx_engine_set_file_wanted(eng, hash.c_str(), files[i].index, wanted);
    }

    // Порядок: архивы, затем отдельные файлы поверх них (общие, потом файлы версии), затем ярлыки.
    const std::vector<int> loose_indices = plan->getLooseFilesForVariant(variant_index, with_optional);
    const std::vector<int> forwarder_indices = plan->forwardersForVariant(variant_index);

    int total_stages = 0;
    if (!archives.empty()) total_stages += static_cast<int>(archives.size());
    if (!loose_indices.empty()) total_stages++;
    total_stages += static_cast<int>(forwarder_indices.size());
    if (total_stages == 0) total_stages = 1;
    int stage_num = 1;

    uint64_t overall_dl_written = 0;
    std::vector<std::string> installed_nros;   // for the post-install check

    // Потоковое скачивание отдельного файла из раздачи по индексу
    auto streamDownloadFile = [&](int file_idx, const std::string& target_path,
                                  uint64_t& cumulative_written, uint64_t total_size) -> bool {
        if (file_idx < 0 || file_idx >= n) return false;
        uint64_t file_size = static_cast<uint64_t>(files[file_idx].size);
        std::string resolved_dest = resolvePlatformPath(target_path);

        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->current_file = std::filesystem::path(resolved_dest).filename().string();
        }

        const bool is_tmp = target_path.rfind(TSNX_CACHE_TMP, 0) == 0;
        auto noteWrite = [&]() {
            size_t slash = resolved_dest.find_last_of("/\\");
            if (slash != std::string::npos) resume->journal.createDirectories(resolved_dest.substr(0, slash));
            resume->journal.noteFile(resolved_dest);
            if (!is_tmp) resume->written.push_back(resolved_dest);
        };

        if (file_size == 0) {
            noteWrite();
            FILE* empty_f = std::fopen(resolved_dest.c_str(), "wb");
            if (empty_f) std::fclose(empty_f);
            return true;
        }

        source->setSchedulerEnabled(true);
        source->setCancelFlag(cancel.get());
        if (cancel->load() || !source->open(hash, file_idx)) {
            st->error = brls::getStr("app/ports/error_open_stream", std::string(files[file_idx].path));
            return false;
        }

        // Only a file this install started may be continued: a shorter file at the
        // same path can be the previous version of the port.
        const bool own_partial = resume->partial_files.count(resolved_dest) > 0;
        noteWrite();
        resume->partial_files.insert(resolved_dest);

        static constexpr size_t kChunk = 2 * 1024 * 1024;
        uint64_t offset = 0;
        FILE* f = nullptr;

        std::error_code ec_exist;
        uint64_t existing_size = std::filesystem::exists(resolved_dest, ec_exist) ? std::filesystem::file_size(resolved_dest, ec_exist) : 0;
        if (own_partial && existing_size > 0 && existing_size < file_size) {
            uint64_t resume_offset = (existing_size / kChunk) * kChunk;
            f = std::fopen(resolved_dest.c_str(), "r+b");
            if (f && fseek64(f, static_cast<int64_t>(resume_offset), SEEK_SET) == 0) {
                offset = resume_offset;
                cumulative_written += resume_offset;
                st->written.store(cumulative_written);
                st->dl_written.store(cumulative_written);
                if (total_size > 0) {
                    float p = std::clamp(static_cast<float>((double)cumulative_written / (double)total_size), 0.0f, 1.0f);
                    st->progress.store(p);
                    st->dl_progress.store(p);
                }
                util::logLine("download: resuming partial port file at offset=" + std::to_string(offset) + " / " + std::to_string(file_size));
            } else {
                if (f) std::fclose(f);
                f = nullptr;
                offset = 0;
            }
        }
        if (!f) {
            f = std::fopen(resolved_dest.c_str(), "wb");
        }
        if (!f) {
            st->error = brls::getStr("app/ports/error_create", resolved_dest);
            source->notifyStreamingComplete(false);
            return false;
        }

        std::vector<unsigned char> fbuf(kChunk);
        std::setvbuf(f, reinterpret_cast<char*>(fbuf.data()), _IOFBF, kChunk);
        std::vector<unsigned char> buf(kChunk);
        bool ok = true;

        while (offset < file_size && !cancel->load() && !g_appExiting.load()) {
            if (pause && pause->load()) {
                std::fflush(f);
                while (pause->load() && !cancel->load() && !g_appExiting.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (cancel->load() || g_appExiting.load()) break;
            }

            size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, file_size - offset));
            size_t got = source->read(offset, buf.data(), want);
            if (got == 0) {
                if (cancel->load() || g_appExiting.load()) break;
                if (pause && pause->load()) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (std::fwrite(buf.data(), 1, got, f) != got) {
                ok = false;
                st->error = brls::getStr("app/ports/error_write", resolved_dest);
                break;
            }
            offset += got;
            cumulative_written += got;
            st->written.store(cumulative_written);
            st->dl_written.store(cumulative_written);
            if (total_size > 0) {
                float p = std::clamp(static_cast<float>((double)cumulative_written / (double)total_size), 0.0f, 1.0f);
                st->progress.store(p);
                st->dl_progress.store(p);
            }
        }
        std::fclose(f);

        if (ok && offset >= file_size) {
            source->notifyStreamingComplete(true);
            resume->partial_files.erase(resolved_dest);
            return true;
        } else {
            source->notifyStreamingComplete(false);
            if (cancel->load() || g_appExiting.load()) {
                std::error_code ec;
                std::filesystem::remove(resolved_dest, ec);
                if (st->error.empty()) st->error = "app/ports/error_cancelled"_i18n;
            } else if (!ok) {
                std::error_code ec;
                std::filesystem::remove(resolved_dest, ec);
            }
            return false;
        }
    };

    // Тома разбитого архива (data.7z.001, .002, ...) читаются как один поток.
    auto openArchive = [&](const installer::PortArchiveTask& a,
                           std::unique_ptr<datasource::ConcatDataSource>& concat,
                           uint64_t& arch_size) -> datasource::IDataSource* {
        std::vector<datasource::ConcatDataSource::Volume> volumes;
        arch_size = 0;
        for (int vi : (a.volume_indices.empty() ? std::vector<int>{a.file_index} : a.volume_indices)) {
            uint64_t vsz = (vi >= 0 && vi < n) ? static_cast<uint64_t>(files[vi].size) : 0;
            volumes.push_back({vi, vsz});
            arch_size += vsz;
        }
        concat.reset();
        datasource::IDataSource* s = source;
        if (volumes.size() > 1) {
            concat = std::make_unique<datasource::ConcatDataSource>(source, hash, volumes);
            s = concat.get();
        }
        source->setSchedulerEnabled(true);
        source->setCancelFlag(cancel.get());
        if (cancel->load() || !s->open(hash, a.file_index)) return nullptr;
        return s;
    };

    // ── ЭТАП 0: ОГЛАВЛЕНИЯ АРХИВОВ (только хвост файла) И СВОБОДНОЕ МЕСТО ──
    if (!archives.empty()) {
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = "app/ports/stage_listing"_i18n;
        }
        std::vector<std::vector<util::ArchiveEntryInfo>> listings(archives.size());
        std::vector<bool> listed(archives.size(), false);
        for (size_t i = 0; i < archives.size() && !cancel->load(); ++i) {
            std::unique_ptr<datasource::ConcatDataSource> concat;
            uint64_t arch_size = 0;
            datasource::IDataSource* s = openArchive(archives[i], concat, arch_size);
            if (!s) continue;
            util::ArchivePeek peek;
            std::string perr;
            if (util::peekArchiveFromSource(s, arch_size, cancel, peek, perr)) {
                listings[i] = std::move(peek.entries);
                listed[i] = true;
                util::logLine("port_installer: listing " + archives[i].archive_torrent_path + " (" + peek.format + "): " +
                              std::to_string(listings[i].size()) + " entries, " +
                              std::to_string(peek.total_uncompressed) + " bytes");
            } else {
                util::logLine("port_installer: no listing for " + archives[i].archive_torrent_path + ": " + perr);
            }
            s->close();
        }
        if (cancel->load()) {
            st->failed = true;
            st->error = "app/ports/error_cancelled"_i18n;
            return;
        }
        // Два прохода: во втором видно, куда легли остальные архивы раздачи.
        installer::PortInstallPlan ctx = *plan;
        ctx.archives_to_extract = archives;
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < archives.size(); ++i) {
                if (listed[i]) installer::refineArchiveDestination(ctx, ctx.archives_to_extract[i], listings[i]);
            }
        }
        archives = ctx.archives_to_extract;
        for (const auto& a : archives) {
            util::logLine("port_installer: " + a.archive_torrent_path + " -> " + a.extract_dest_dir +
                          (a.strip_prefix.empty() ? "" : " (strip '" + a.strip_prefix + "')") +
                          (a.dest_reason.empty() ? "" : " [" + a.dest_reason + "]"));
        }
    }

    {
        // Место на SD: распакованные архивы (если оглавление прочитано, иначе сжатый размер),
        // отдельные файлы и временный NSP; плюс запас.
        uint64_t need = 0;
        for (const auto& a : archives) {
            need += a.uncompressed_size > 0 ? a.uncompressed_size : static_cast<uint64_t>(a.compressed_size);
        }
        for (int idx : loose_indices) {
            if (idx >= 0 && idx < n) need += static_cast<uint64_t>(files[idx].size);
        }
        for (int idx : forwarder_indices) {
            if (idx >= 0 && idx < n) need += static_cast<uint64_t>(files[idx].size);
        }
        need += need / 50 + 64ULL * 1024 * 1024;
        int64_t sd_free = 0;
        if (util::getStorageFreeSpace(1, sd_free) && sd_free >= 0 && static_cast<uint64_t>(sd_free) < need) {
            st->failed = true;
            st->error = brls::getStr("app/ports/error_space", std::to_string(need / (1024 * 1024)) + " MB",
                                     std::to_string(static_cast<uint64_t>(sd_free) / (1024 * 1024)) + " MB");
            util::logLine("port_installer: " + st->error);
            return;
        }
        util::logLine("port_installer: space check need=" + std::to_string(need) + " free=" + std::to_string(sd_free));
    }

    // ── ЭТАП 1: ПОТОКОВАЯ РАСПАКОВКА И УСТАНОВКА АРХИВОВ НА ЛЕТУ ──
    if (!archives.empty()) {
        for (const auto& a : archives) {
            if (cancel->load()) break;

            std::string arch_filename = std::filesystem::path(a.archive_torrent_path).filename().string();
            if (resume->done_archives.count(a.file_index)) {
                // Retry: already extracted.
                overall_dl_written += static_cast<uint64_t>(a.compressed_size);
                st->dl_written.store(overall_dl_written);
                util::logLine("port_installer: " + arch_filename + " already extracted, skipped");
                stage_num++;
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(st->msg_mutex);
                st->stage_label = brls::getStr("app/ports/stage_extracting", arch_filename, std::to_string(stage_num), std::to_string(total_stages));
                st->current_file = arch_filename;
            }
            st->total.store(total_dl_bytes);
            st->dl_total.store(total_dl_bytes);

            std::string resolved_extract_dir = resolvePlatformPath(a.extract_dest_dir);
            util::safeCreateDirectories(resolved_extract_dir);

            std::unique_ptr<datasource::ConcatDataSource> concat;
            uint64_t arch_file_size = 0;
            datasource::IDataSource* arch_source = openArchive(a, concat, arch_file_size);
            if (!arch_source) {
                st->failed = true;
                st->error = brls::getStr("app/ports/error_open_stream", a.archive_torrent_path);
                return;
            }
            const size_t volume_count = a.volume_indices.empty() ? 1 : a.volume_indices.size();

            // Пути внутри архива: снять обёртку, оставить подпапку рецепта, не писать в
            // bootloader/, Nintendo/, atmosphere/ (кроме contents/) и т.п.
            int protected_skipped = 0;
            auto protectTransform = [&](const std::string& entryName, bool isDir) -> std::string {
                std::string target = installer::archiveEntryTarget(a, entryName);
                if (target.empty()) {
                    if (protected_skipped++ < 20) util::logLine("port_installer: skipped archive entry " + entryName);
                    return target;
                }
                // The install journal (rollback on cancel) and the installed-files list.
                std::string full = resolved_extract_dir;
                if (!full.empty() && full.back() != '/') full += '/';
                full += target;
                if (isDir) {
                    resume->journal.createDirectories(full);
                } else {
                    size_t slash = full.find_last_of('/');
                    if (slash != std::string::npos) resume->journal.createDirectories(full.substr(0, slash));
                    resume->journal.noteFile(full);
                    resume->written.push_back(full);
                    if (full.size() > 4 && util::equalsIgnoreCase(full.substr(full.size() - 4), ".nro")) {
                        installed_nros.push_back(full);
                    }
                }
                return target;
            };

            auto extractProgressCb = [&](const util::ArchiveProgress& prog) {
                if (cancel->load()) return;
                {
                    std::lock_guard<std::mutex> lk(st->msg_mutex);
                    if (!prog.currentFileName.empty()) {
                        st->current_file = prog.currentFileName;
                    }
                }
                // A streamed ZIP gives no unpacked total, so the bar stood at 0:
                // take it from the listing read before the install.
                const uint64_t unpacked_total = prog.totalUncompressedSize > 0 ? prog.totalUncompressedSize
                                                                               : a.uncompressed_size;
                uint64_t inst_w = prog.bytesExtracted;
                uint64_t inst_t = unpacked_total;
                float norm_pct = std::clamp(prog.percentage / 100.0f, 0.0f, 1.0f);
                if (unpacked_total > 0) {
                    norm_pct = std::clamp(static_cast<float>(static_cast<double>(prog.bytesExtracted) /
                                                             static_cast<double>(unpacked_total)), 0.0f, 1.0f);
                } else if (arch_file_size > 0) {
                    // Nothing known: show the share of the archive read so far.
                    inst_t = arch_file_size;
                    inst_w = static_cast<uint64_t>(norm_pct * static_cast<double>(arch_file_size));
                }
                st->written.store(inst_w);
                st->total.store(inst_t);
                st->progress.store(norm_pct);

                st->inst_written.store(inst_w);
                st->inst_total.store(inst_t);
                st->inst_progress.store(norm_pct);

                if (arch_file_size > 0 && prog.totalArchiveSize > 0) {
                    uint64_t cur_arch_read = static_cast<uint64_t>((static_cast<double>(prog.percentage) / 100.0) * arch_file_size);
                    st->dl_written.store(overall_dl_written + cur_arch_read);
                    if (total_dl_bytes > 0) {
                        st->dl_progress.store(std::clamp(static_cast<float>((double)(overall_dl_written + cur_arch_read) / (double)total_dl_bytes), 0.0f, 1.0f));
                    }
                }
            };

            std::string extractErr;
            util::logLine("port_installer: streaming extraction " + arch_filename + " (" +
                          std::to_string(arch_file_size) + " bytes, " + std::to_string(volume_count) +
                          " volume(s)) -> " + resolved_extract_dir);

            bool extractOk = util::extractArchiveFromSource(
                arch_source, arch_file_size, resolved_extract_dir,
                extractProgressCb, cancel, pause, extractErr, protectTransform
            );

            arch_source->notifyStreamingComplete(extractOk);
            if (concat) {
                util::logLine("port_installer: volume switches=" + std::to_string(concat->volumeSwitches()));
            }
            if (protected_skipped > 0) {
                util::logLine("port_installer: " + std::to_string(protected_skipped) +
                              " archive entries were not written (outside the subfolder or protected)");
            }

            if (!extractOk) {
                st->failed = true;
                st->error = brls::getStr("app/ports/error_extract", extractErr);
                util::logLine("port_installer: " + st->error);
                return;
            }

            resume->done_archives.insert(a.file_index);
            overall_dl_written += arch_file_size;
            st->dl_written.store(overall_dl_written);
            if (total_dl_bytes > 0) {
                st->dl_progress.store(std::clamp(static_cast<float>((double)overall_dl_written / (double)total_dl_bytes), 0.0f, 1.0f));
            }
            stage_num++;
        }
    }

    if (cancel->load()) {
        st->failed = true;
        st->error = "app/ports/error_cancelled"_i18n;
        return;
    }

    // ── ЭТАП 3: СКАЧИВАНИЕ LOOSE И OVERLAY ФАЙЛОВ (.NRO / РУСИФИКАТОРЫ) ──
    if (!loose_indices.empty()) {
        int current_stage = std::min(stage_num, total_stages);
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = brls::getStr("app/ports/stage_files", std::to_string(current_stage), std::to_string(total_stages));
        }

        for (int idx : loose_indices) {
            if (cancel->load()) break;
            auto dit = plan->file_destinations.find(idx);
            if (dit == plan->file_destinations.end() || dit->second.empty()) {
                util::logLine("port_installer: no destination for file #" + std::to_string(idx) + ", skipped");
                continue;
            }
            const std::string resolved = resolvePlatformPath(dit->second);
            if (resume->done_files.count(idx)) {
                std::error_code ec;
                if (std::filesystem::exists(resolved, ec) &&
                    std::filesystem::file_size(resolved, ec) == static_cast<uint64_t>(files[idx].size)) {
                    overall_dl_written += static_cast<uint64_t>(files[idx].size);
                    continue;   // retry: already copied
                }
                resume->done_files.erase(idx);
            }
            if (!streamDownloadFile(idx, dit->second, overall_dl_written, total_dl_bytes)) {
                st->failed = true;
                return;
            }
            resume->done_files.insert(idx);
            if (resolved.size() > 4 && util::equalsIgnoreCase(resolved.substr(resolved.size() - 4), ".nro")) {
                installed_nros.push_back(resolved);
            }
        }
        stage_num++;
    }

    if (cancel->load()) {
        st->failed = true;
        st->error = "app/ports/error_cancelled"_i18n;
        return;
    }

    // ── ЭТАП 4: СКАЧИВАНИЕ И УСТАНОВКА ЯРЛЫКОВ FORWARDER NSP (по одному на версию) ──
    for (int forwarder_nsp_idx : forwarder_indices) {
        if (cancel->load() || st->failed.load()) break;
        const int current_stage = std::min(stage_num, total_stages);
        const std::string nsp_name = (forwarder_nsp_idx >= 0 && forwarder_nsp_idx < n)
            ? std::filesystem::path(files[forwarder_nsp_idx].path).filename().string()
            : plan->forwarder_nsp_name;

        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = brls::getStr("app/ports/stage_forwarder_download", std::to_string(current_stage), std::to_string(total_stages));
            st->current_file = nsp_name;
        }

        if (resume->done_forwarders.count(forwarder_nsp_idx)) {
            util::logLine("port_installer: forwarder " + nsp_name + " already installed, skipped");
            stage_num++;
            continue;
        }

        auto nit = plan->file_destinations.find(forwarder_nsp_idx);
        std::string nsp_dest = nit != plan->file_destinations.end() ? nit->second : std::string();
        if (nsp_dest.empty()) {
            nsp_dest = std::string(TSNX_CACHE_TMP) + "/" + nsp_name;
        }
        // The temporary NSP is removed on every way out of this iteration.
        struct TmpFileGuard {
            std::string path;
            ~TmpFileGuard() {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        } tmp_guard{resolvePlatformPath(nsp_dest)};

        if (!streamDownloadFile(forwarder_nsp_idx, nsp_dest, overall_dl_written, total_dl_bytes)) {
            st->failed = true;
            return;
        }

        if (cancel->load()) {
            st->failed = true;
            st->error = "app/ports/error_cancelled"_i18n;
            return;
        }

        // Установка forwarder NSP в операционную систему Switch
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = brls::getStr("app/ports/stage_forwarder_install", std::to_string(current_stage), std::to_string(total_stages));
            st->current_file = nsp_name;
        }

        std::string resolved_nsp = resolvePlatformPath(nsp_dest);
        datasource::FileDataSource nspSource(resolved_nsp);
        nspSource.setCancelFlag(cancel.get());
        if (!nspSource.open()) {
            st->failed = true;
            st->error = brls::getStr("app/ports/error_forwarder", resolved_nsp);
            util::logLine("port_installer: " + st->error);
            return;
        }

        installer::InstallConfig config;
        config.buffer_size = 16 * 1024 * 1024;
        config.chunk_size = 1 * 1024 * 1024;
        config.verify_sha256 = true;
        config.install_ticket = true;

        installer::HybridNspInstaller nspInstaller;
        const uint64_t nsp_tid = download::parseTitleIdFromFileName(nsp_name);
        if (nsp_tid != 0) nspInstaller.setHintTitleId(nsp_tid);
        if (nspInstaller.start(&nspSource, config)) {
            while (!nspInstaller.isFinished() && !cancel->load()) {
                nspInstaller.setPaused(pause && pause->load());
                float p = std::clamp(nspInstaller.progress(), 0.0f, 1.0f);
                st->progress.store(p);
                st->written.store(nspInstaller.bytesInstalled());
                st->total.store(nspInstaller.totalBytes());

                st->inst_progress.store(p);
                st->inst_written.store(nspInstaller.bytesInstalled());
                st->inst_total.store(nspInstaller.totalBytes());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (cancel->load()) {
                nspInstaller.cancel();
                st->failed = true;
                st->error = "app/ports/error_cancelled"_i18n;
            } else if (nspInstaller.state() == installer::InstallState::Failed) {
                st->failed = true;
                st->error = brls::getStr("app/ports/error_forwarder", nspInstaller.errorMessage());
                util::logLine("port_installer: " + st->error);
            } else {
                util::logLine("port_installer: forwarder NSP successfully installed!");
                resume->done_forwarders.insert(forwarder_nsp_idx);
                if (nsp_tid != 0) resume->forwarder_tids.push_back(nsp_tid);
            }
        } else {
            st->failed = true;
            st->error = brls::getStr("app/ports/error_forwarder", nspInstaller.errorMessage());
            util::logLine("port_installer: " + st->error);
        }

        nspSource.close();
        stage_num++;
    }

    if (!st->failed.load() && !cancel->load()) {
        // Проверка: файлы из рецепта на месте; для эвристики только подсказки.
        const auto& variant = plan->variants[variant_index];
        for (const auto& check : variant.check_paths) {
            std::error_code ec;
            if (!std::filesystem::exists(resolvePlatformPath(check), ec)) {
                st->failed = true;
                st->error = brls::getStr("app/ports/error_check_missing", check);
                util::logLine("port_installer: post-install check failed: " + check);
                return;
            }
        }
        std::string note = plan->note;
        if (forwarder_indices.empty()) {
            if (note.empty()) note = brls::getStr("app/ports/note_no_forwarder");
        } else if (installed_nros.empty()) {
            note += (note.empty() ? "" : "\n") + brls::getStr("app/ports/note_no_nro");
            util::logLine("port_installer: warning: a forwarder was installed but no .nro was written");
        }

        // Манифест установленного порта: «Установлено», обновление, удаление.
        installer::InstalledPort record;
        record.topic_id = topic_id;
        record.title = release_title;
        record.key = installer::InstalledPorts::keyFor(topic_id, release_title);
        record.variant_id = variant.id;
        record.variant_title = variant.title_key.empty() ? variant.title
            : (variant.title_arg.empty() ? brls::getStr(variant.title_key) : brls::getStr(variant.title_key, variant.title_arg));
        record.game = plan->port_game_name;
        record.recipe_id = plan->recipe_id;
        record.installed_at = std::time(nullptr);
        record.files = resume->written;
        record.forwarder_tids = resume->forwarder_tids;
        installer::InstalledPorts::instance().saveReplacing(record);

        // Готово: следующий «Повторить» начнёт с нуля.
        resume->journal = util::ExtractionJournal();
        resume->written.clear();
        resume->done_archives.clear();
        resume->done_files.clear();
        resume->done_forwarders.clear();
        resume->forwarder_tids.clear();
        resume->partial_files.clear();
        resume->variant = -1;

        st->done = true;
        st->progress.store(1.0f);
        st->dl_progress.store(1.0f);
        st->inst_progress.store(1.0f);
        if (total_dl_bytes > 0) st->dl_written.store(total_dl_bytes);
        {
            std::lock_guard<std::mutex> lk(st->msg_mutex);
            st->stage_label = "app/ports/stage_done"_i18n;
            st->current_file = "app/ports/stage_done_file"_i18n;
            st->result_note = note;
        }
        util::logLine("port_installer: successfully finished all phases for " + release_title +
                      (note.empty() ? "" : " | note: " + note));
    }
}

// Нужен ли ещё этот торрент другим элементам очереди (чтобы не чистить кеш).
static bool torrentStillNeeded(const std::vector<download::DownloadItem>& queue,
                               size_t self_index, const std::string& hash) {
    if (hash.empty()) return false;
    for (size_t j = 0; j < queue.size(); ++j) {
        if (j == self_index) continue;
        const auto& other = queue[j];
        std::string other_hash = other.torrent_hash;
        if (other_hash.empty() && !other.magnet.empty()) {
            other_hash = extractBtihHash(normalizeTorrentLink(other.magnet));
        }
        if (other_hash == hash &&
            (other.state == download::DownloadState::Queued ||
             other.state == download::DownloadState::Downloading ||
             other.state == download::DownloadState::StreamPreparing ||
             other.state == download::DownloadState::StreamInstalling)) {
            return true;
        }
    }
    return false;
}

bool DownloadManager::isLocalBackend() const {
    return ds_manager_.mode() == datasource::DataSourceMode::LocalClient ||
           ds_manager_.mode() == datasource::DataSourceMode::CustomEngine;
}

static void updateSleepPolicy(bool has_active_transfers) {
    static bool last_boosted = false;
    if (has_active_transfers != last_boosted) {
        if (has_active_transfers) {
            util::cpuBoostBegin();
        } else {
            util::cpuBoostEnd();
        }
        last_boosted = has_active_transfers;
    }
#ifdef __SWITCH__
    static bool initialized = false;
    static bool last_keep_awake = false;

    const bool enabled = config::ConfigManager::instance().getKeepAwakeDuringDownloads();
    const bool keep_awake = enabled && has_active_transfers;
    if (initialized && keep_awake == last_keep_awake) {
        return;
    }

    appletSetMediaPlaybackState(keep_awake);
    util::logLine(std::string("power: keep-awake ") + (keep_awake ? "enabled" : "disabled"));
    last_keep_awake = keep_awake;
    initialized = true;
#else
    (void)has_active_transfers;
#endif
}

DownloadManager::DownloadManager() {
    progress_thread_running_.store(true);
    progress_thread_ = std::thread([this]() {
        // Off the UI core: this thread polls the engine and holds the queue
        // mutex the UI also takes (see thread_cfg.h).
        tsnx_thread_pin_self(TSNX_CORE_NET, TSNX_PRIO_PROGRESS);
        while (progress_thread_running_.load()) {
            try {
                trackProgress();
            } catch (const std::exception& e) {
                util::logLine("DownloadManager: exception in trackProgress: " + std::string(e.what()));
            } catch (...) {
                util::logLine("DownloadManager: unknown exception in trackProgress");
            }
            
            if (progress_callback_) {
                try {
                    progress_callback_();
                } catch (...) {}
            }

            std::unique_lock<std::mutex> lock(progress_cv_mutex_);
            if (has_open_pending_.load()) {
                progress_cv_.wait_for(lock, std::chrono::milliseconds(100));
            } else {
                progress_cv_.wait_for(lock, std::chrono::milliseconds(1000));
            }
        }
    });
}

DownloadManager::~DownloadManager() {
    shutdown();
}

void DownloadManager::shutdown() {
    g_appExiting.store(true);
    datasource::CustomEngineClient::instance().cancelProbe();

    if (progress_thread_running_.load()) {
        progress_thread_running_.store(false);
        progress_cv_.notify_all();
        if (progress_thread_.joinable()) {
            progress_thread_.join();
        }
    }

    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);

    for (auto& item : queue_) {
        if (item.cancel_flag) {
            item.cancel_flag->store(true);
        }
        if (item.file_dl_cancel) {
            item.file_dl_cancel->store(true);
        }
        if (item.file_dl_pause) {
            item.file_dl_pause->store(false);
        }
        if (item.port_install_cancel) {
            item.port_install_cancel->store(true);
        }
        if (item.port_install_pause) {
            item.port_install_pause->store(false);
        }
        if (item.hybrid_installer) {
            item.hybrid_installer->cancel();
        }
    }

    has_open_pending_.store(false);

    stopAllStreamConsumers();

    datasource::IDataSource* source = ds_manager_.getSource();
    if (source) {
        source->close();
    }

    // source->close() выше разблокирует заблокированные в source->read() воркеры
    // (torrentfs teardown); только после этого join'им их фьючерсы, чтобы
    // завершение не повисло.
    for (auto& item : queue_) {
        if (item.open_future) {
            item.open_future.reset();
        }
        if (item.start_future) {
            item.start_future.reset();
        }
        if (item.hybrid_installer) {
            item.hybrid_installer.reset();
        }
        if (item.file_dl_worker) {
            item.file_dl_worker.reset();
        }
        if (item.port_install_worker) {
            item.port_install_worker.reset();
        }
        item.file_dl_dispatched = false;
        item.file_dl_state.reset();
        item.port_install_dispatched = false;
        item.port_install_state.reset();
    }
    retired_port_workers_.clear();   // joins cancelled port workers (the source is closed)

    updateSleepPolicy(false);
    datasource::CustomEngineClient::instance().shutdown();
}

size_t DownloadManager::addToQueue(const std::string& title,
                                   const std::string& magnet,
                                   int forced_file_index,
                                   const std::string& forced_stream_name,
                                   bool is_homebrew,
                                   const std::string& retro_console_id) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    DownloadItem item;
    item.title = title;
    item.magnet = magnet;
    item.forced_file_index = forced_file_index;
    item.forced_stream_name = forced_stream_name;
    item.is_homebrew = is_homebrew;
    item.retro_console_id = retro_console_id;
    item.state = DownloadState::Queued;
    if (isLocalBackend()) {
        std::string local_hash = extractBtihHash(normalizeTorrentLink(magnet));
        if (!local_hash.empty()) {
            item.torrent_hash = local_hash;
            datasource::CustomEngineClient::instance().markInUse(local_hash);
        }
    }
    queue_.push_back(std::move(item));
    return queue_.size() - 1;
}

size_t DownloadManager::addPortInstall(const std::string& title,
                                       const std::string& magnet,
                                       std::shared_ptr<installer::PortInstallPlan> plan,
                                       int variant_index,
                                       bool with_optional) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    DownloadItem item;
    item.title = title;
    item.magnet = magnet;
    item.is_port_auto_install = true;
    item.port_plan = plan;
    item.selected_variant_index = variant_index;
    item.port_include_optional = with_optional;
    if (plan) {
        item.selected_files = plan->getFilesForVariant(variant_index, with_optional);
    }
    item.state = DownloadState::Queued;
    item.install_stage_label = "app/ports/stage_queued"_i18n;
    if (isLocalBackend()) {
        std::string local_hash = extractBtihHash(normalizeTorrentLink(magnet));
        if (!local_hash.empty()) {
            item.torrent_hash = local_hash;
            datasource::CustomEngineClient::instance().markInUse(local_hash);
        }
    }
    queue_.push_back(std::move(item));
    return queue_.size() - 1;
}

static bool getFileSize(const std::string& path, uint64_t& size_out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    size_out = (uint64_t)st.st_size;
    return true;
}

static size_t readChunkToInstaller(const std::string& path, uint64_t& offset, installer::StreamInstaller& installer, size_t max_bytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return 0;
    }

    static std::vector<unsigned char> buf(1024 * 1024);
    size_t to_read = max_bytes < buf.size() ? max_bytes : buf.size();
    size_t n = std::fread(buf.data(), 1, to_read, f);
    std::fclose(f);
    if (n > 0) {
        installer.readChunk(buf.data(), n);
        offset += n;
    }
    return n;
}

static void replaceAllInPlace(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string normalizeTorrentLink(std::string link) {
    // Catalogs often escape query separators; recover a usable magnet URL.
    replaceAllInPlace(link, "&amp;", "&");
    replaceAllInPlace(link, "&#38;", "&");
    replaceAllInPlace(link, "\\u0026", "&");
    replaceAllInPlace(link, "\\u002F", "/");
    replaceAllInPlace(link, "\\/", "/");
    return link;
}

static std::string extractBtihHash(std::string magnet) {
    std::transform(magnet.begin(), magnet.end(), magnet.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const std::string marker = "xt=urn:btih:";
    size_t pos = magnet.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    size_t end = magnet.find('&', pos);
    if (end == std::string::npos) end = magnet.size();
    if (end <= pos) return {};
    return magnet.substr(pos, end - pos);
}

static bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

uint64_t parseTitleIdFromFileName(const std::string& name) {
    return util::parseTitleIdFromFileName(name);
}

int installFilePriority(const std::string& name) {
    std::string basename = name;
    size_t slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) {
        basename = basename.substr(slash + 1);
    }

    std::string lower = basename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    int format_score = 0;
    bool is_xci = false;
    if (lower.size() >= 4 && lower.rfind(".nsp") == lower.size() - 4) {
        format_score = 4;
    } else if (lower.size() >= 4 && lower.rfind(".nsz") == lower.size() - 4) {
        format_score = 4; // Равный приоритет с .nsp: формат не должен перебивать базовую игру!
    } else if (lower.size() >= 4 && lower.rfind(".xci") == lower.size() - 4) {
        format_score = 3;
        is_xci = true;
    } else if (lower.size() >= 4 && lower.rfind(".xcz") == lower.size() - 4) {
        format_score = 3;
        is_xci = true;
    } else if (lower.size() >= 5 && lower.rfind(".pfs0") == lower.size() - 5) {
        format_score = 1;
    } else {
        return 0; // Не поддерживаемый формат Switch пакета
    }

    // 1. Проверяем Title ID в имени файла
    uint64_t tid = parseTitleIdFromFileName(basename);
    bool is_base_tid = false;
    bool is_update_tid = false;
    bool is_dlc_tid = false;

    if (tid != 0) {
        uint64_t type_bits = tid & 0xFFFULL;
        if (type_bits == 0) {
            is_base_tid = true; // Application / Base game
        } else if (type_bits == 0x800) {
            is_update_tid = true; // Patch / Update
        } else {
            is_dlc_tid = true; // AddOnContent / DLC
        }
    }

    // 2. Текстовые маркеры v0 / Base
    bool is_explicit_v0 = false;
    if (lower.find("[v0]") != std::string::npos ||
        lower.find("(v0)") != std::string::npos ||
        lower.find("[v0.") != std::string::npos ||
        lower.find("(v0.") != std::string::npos ||
        lower.find("_v0.") != std::string::npos ||
        lower.find("_v0_") != std::string::npos ||
        lower.find("_v0 ") != std::string::npos ||
        lower.find(" v0.") != std::string::npos ||
        lower.find(" v0 ") != std::string::npos ||
        lower.find(" v0]") != std::string::npos ||
        lower.find("[base]") != std::string::npos ||
        lower.find("(base)") != std::string::npos ||
        lower.find("_base_") != std::string::npos ||
        lower.find("_base.") != std::string::npos ||
        lower.find(" base ") != std::string::npos ||
        lower.find("[base game]") != std::string::npos ||
        lower.find("(base game)") != std::string::npos) {
        is_explicit_v0 = true;
    }

    // 3. Текстовые маркеры Update
    bool is_explicit_update = false;
    if (lower.find("[upd") != std::string::npos ||
        lower.find("(upd") != std::string::npos ||
        lower.find("[update") != std::string::npos ||
        lower.find("(update") != std::string::npos ||
        lower.find("_upd") != std::string::npos ||
        lower.find("_update") != std::string::npos ||
        lower.find(" update ") != std::string::npos ||
        lower.find("[patch") != std::string::npos ||
        lower.find("(patch") != std::string::npos ||
        lower.find("_patch") != std::string::npos ||
        lower.find(" patch ") != std::string::npos) {
        is_explicit_update = true;
    }

    // Проверяем [vXXXXX] где версия > 0 (патчи часто имеют [v65536], [v131072] и т.д.)
    if (!is_explicit_v0 && !is_explicit_update) {
        size_t vpos = lower.find("[v");
        if (vpos != std::string::npos && vpos + 2 < lower.size()) {
            if (std::isdigit(static_cast<unsigned char>(lower[vpos + 2])) && lower[vpos + 2] != '0') {
                is_explicit_update = true;
            }
        }
    }

    // 4. Текстовые маркеры DLC
    bool is_explicit_dlc = false;
    if (lower.find("[dlc") != std::string::npos ||
        lower.find("(dlc") != std::string::npos ||
        lower.find("_dlc") != std::string::npos ||
        lower.find(" dlc ") != std::string::npos ||
        lower.find("[addon") != std::string::npos) {
        is_explicit_dlc = true;
    }

    // Приоритезация:
    // 1. DLC (даже если помечен как v0, DLC не является исполняемой базовой игрой)
    if (is_explicit_dlc || (is_dlc_tid && !is_base_tid)) {
        return 100 + format_score;
    }

    // 2. Обновление (патч не может быть базовой игрой)
    if (is_explicit_update || (is_update_tid && !is_base_tid)) {
        return 200 + format_score;
    }

    // 3. Базовая игра (v0 / Application) — ВСЕГДА на первом месте!
    if (is_explicit_v0 && is_base_tid) {
        return 1100 + format_score;
    }
    if (is_explicit_v0 || is_base_tid) {
        return 1000 + format_score;
    }

    // 4. Картридж XCI/XCZ обычно содержит базовую игру
    if (is_xci) {
        return 900 + format_score;
    }

    // 5. Неизвестный файл (нет явных признаков ни v0, ни update, ни DLC)
    return 500 + format_score;
}

static std::string ensureHttpUrl(std::string url) {
    if (url.empty()) return "http://127.0.0.1:8090";
    if (url.find("://") == std::string::npos) {
        url = "http://" + url;
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

static std::string urlEncodeLocal(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
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

static void copyDownloadedOtherFiles(const download::DownloadItem& item) {
    std::filesystem::path srcDir = std::filesystem::path(TSNX_CACHE_LOCALENGINE) / item.torrent_hash;

    std::filesystem::path destDir = std::filesystem::path(TSNX_DOWNLOADS_DIR);

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    // Custom engine streams into RAM and does not keep a local SD cache,
    // so there are no "other files" to copy after installation.
    (void)srcDir; (void)destDir;
}

static void clearTorrentCache(const std::string& hash) {
    if (hash.empty()) return;

    std::filesystem::path cachePath = std::filesystem::path(TSNX_CACHE_LOCALENGINE) / hash;

    std::error_code ec;
    if (std::filesystem::exists(cachePath)) {
        util::logLine("download: explicitly deleting cache path " + cachePath.string());
        std::filesystem::remove_all(cachePath, ec);
        if (ec) {
            util::logLine("download: failed to delete cache path: " + ec.message());
        }
    }
}

// Removing a torrent joins its engine threads (the discovery thread may be in
// a tracker announce or a DNS lookup: up to seconds) and deletes its cache.
// trackProgress() runs under the queue mutex the UI also takes, so there the
// removal is only queued here and carried out by the same progress thread
// right after the mutex is released (DeferredReleaseScope). Other callers --
// the user's own cancel/delete actions -- still release synchronously.
static std::atomic<std::vector<std::string>*> g_deferred_releases{nullptr};
static std::atomic<std::thread::id> g_deferred_owner{};

class DeferredReleaseScope {
public:
    explicit DeferredReleaseScope(std::vector<std::string>& sink) {
        g_deferred_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
        g_deferred_releases.store(&sink, std::memory_order_release);
    }
    ~DeferredReleaseScope() { g_deferred_releases.store(nullptr, std::memory_order_release); }
    DeferredReleaseScope(const DeferredReleaseScope&) = delete;
    DeferredReleaseScope& operator=(const DeferredReleaseScope&) = delete;
};

static void removeTorrentNow(const std::string& hash) {
    tsnx_engine* eng = datasource::CustomEngineClient::instance().sharedEngine();
    if (eng) {
        tsnx_engine_remove_torrent(eng, hash.c_str());
    }
    clearTorrentCache(hash);
    util::logLine("download: released unneeded torrent and cleared cache, hash=" + hash);
}

// Called by the progress thread with the queue mutex released.
static void runDeferredReleases(const std::vector<std::string>& hashes) {
    for (const auto& hash : hashes) {
        // Re-adopted (retry / re-added) between the decision and now: keep it.
        if (datasource::CustomEngineClient::instance().isInUse(hash)) {
            util::logLine("download: deferred release skipped, torrent re-adopted, hash=" + hash);
            continue;
        }
        removeTorrentNow(hash);
    }
}

static void releaseTorrentIfUnneeded(const std::vector<download::DownloadItem>& queue,
                                     size_t self_index, const std::string& hash) {
    if (hash.empty()) return;
    if (!torrentStillNeeded(queue, self_index, hash)) {
        datasource::CustomEngineClient::instance().unmarkInUse(hash);
        std::vector<std::string>* deferred = g_deferred_releases.load(std::memory_order_acquire);
        if (deferred &&
            g_deferred_owner.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
            if (std::find(deferred->begin(), deferred->end(), hash) == deferred->end())
                deferred->push_back(hash);
            util::logLine("download: torrent release queued until the queue lock is free, hash=" + hash);
        } else {
            removeTorrentNow(hash);
        }
    } else {
        util::logLine("download: keeping torrent active as it is needed by another queued item, hash=" + hash);
    }
}

static std::string extractBaseTopicId(const std::string& tid) {
    if (tid.empty()) return "";
    size_t pos = tid.find('_');
    return (pos != std::string::npos) ? tid.substr(0, pos) : tid;
}

static bool isSameReleaseItem(const download::DownloadItem& a, const download::DownloadItem& b) {
    if (!a.torrent_hash.empty() && !b.torrent_hash.empty() && a.torrent_hash == b.torrent_hash) {
        return true;
    }
    if (!a.magnet.empty() && !b.magnet.empty() && a.magnet == b.magnet) {
        return true;
    }
    std::string baseA = extractBaseTopicId(a.topic_id);
    std::string baseB = extractBaseTopicId(b.topic_id);
    if (!baseA.empty() && !baseB.empty() && baseA == baseB) {
        return true;
    }
    return false;
}

static void checkAndNotifyReleaseComplete(const std::vector<download::DownloadItem>& queue,
                                          const download::DownloadItem& completed_item) {
    bool all_done = true;
    for (const auto& other : queue) {
        if (isSameReleaseItem(completed_item, other)) {
            if (other.state == download::DownloadState::Queued ||
                other.state == download::DownloadState::Downloading ||
                other.state == download::DownloadState::StreamPreparing ||
                other.state == download::DownloadState::StreamInstalling ||
                other.state == download::DownloadState::Installing ||
                other.state == download::DownloadState::Paused) {
                all_done = false;
                break;
            }
        }
    }

    if (all_done) {
        std::string title = cleanTitle(completed_item.title);
        size_t paren = title.find(" (");
        if (paren != std::string::npos && paren > 0) {
            title = title.substr(0, paren);
        }
        size_t bracket = title.find(" [");
        if (bracket != std::string::npos && bracket > 0) {
            std::string suffix = title.substr(bracket);
            if (suffix.find("DLC") != std::string::npos ||
                suffix.find("Update") != std::string::npos ||
                suffix.find("v") != std::string::npos) {
                title = title.substr(0, bracket);
            }
        }
        util::logLine("download: all files in release completed: " + title);
        brls::sync([title]() {
            std::string msg = brls::getStr("app/downloads/release_completed", title);
            brls::Application::notify(msg);
        });
    }
}

static std::string streamRouteNameFromPath(const std::string& path_or_name, const std::string& fallback) {
    std::string name = path_or_name;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    if (name.empty()) {
        name = fallback;
    }
    return name;
}

static bool chooseInstallFile(const std::vector<torrent::TorrentFileInfo>& files,
                               int& out_index,
                               std::string& out_stream_name) {
    if (files.empty()) return false;

    int chosen_index = -1;
    std::string chosen_stream_name;
    unsigned long long chosen_size = 0;
    int chosen_priority = -1;

    for (const auto& f : files) {
        const int priority = installFilePriority(f.name);
        if (priority <= 0) {
            continue;
        }
        if (chosen_index < 0 ||
            priority > chosen_priority ||
            (priority == chosen_priority && f.size > chosen_size)) {
            chosen_priority = priority;
            chosen_size = f.size;
            chosen_index = f.index;
            chosen_stream_name = f.name;
        }
    }

    if (chosen_index < 0) {
        return false;
    }

    out_index = chosen_index;
    out_stream_name = chosen_stream_name;
    return true;
}

static std::string buildStreamPlayUrl(const std::string& base_url,
                                      const std::string& route_name,
                                      const std::string& hash,
                                      int file_index) {
    return base_url + "/stream/" + urlEncodeLocal(route_name) + "?link=" + urlEncodeLocal(hash) +
           "&index=" + std::to_string(file_index) + "&play";
}

static std::string buildStreamPreloadUrl(const std::string& base_url,
                                         const std::string& route_name,
                                         const std::string& hash,
                                         int file_index) {
    return base_url + "/stream/" + urlEncodeLocal(route_name) + "?link=" + urlEncodeLocal(hash) +
           "&index=" + std::to_string(file_index) + "&preload";
}

static float smoothDownloadSpeedKbps(float displayed_kbps,
                                     float sampled_kbps,
                                     std::chrono::steady_clock::time_point last_sample_at,
                                     std::chrono::steady_clock::time_point now) {
    if (!(sampled_kbps >= 0.0f)) sampled_kbps = 0.0f;
    if (!(displayed_kbps >= 0.0f)) displayed_kbps = 0.0f;

    if (last_sample_at.time_since_epoch().count() == 0 || displayed_kbps == 0.0f) {
        return sampled_kbps;
    }

    double dt_sec = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample_at).count() / 1000.0;
    if (dt_sec <= 0.0) dt_sec = 0.8;

    // Use a much larger smoothing window (20 seconds) to mask short-term
    // disk IO / OS stalls so the instantaneous speed does not drop to 0.
    const double window_sec = 20.0;
    double alpha = dt_sec / window_sec;
    if (alpha < 0.02) alpha = 0.02;
    if (alpha > 0.15) alpha = 0.15;

    return static_cast<float>(displayed_kbps + (sampled_kbps - displayed_kbps) * alpha);
}

static constexpr uint64_t kPumpStartOffset = 28ull * 1024ull * 1024ull;
static constexpr uint64_t kPumpRewindBytes = 1024ull * 1024ull;
static constexpr uint64_t kPumpChunkBytes = 8ull * 1024ull * 1024ull;
static constexpr int kPumpTimeoutSec = 6;
static constexpr size_t kLocalHeaderProbeBytes = 4096;
static constexpr int kPumpIdleSleepMs = 250;
static constexpr int kPumpRepreloadEveryZeroReads = 12;
static constexpr int kPumpRewindEveryZeroReads = 24;

torrent::TorrentManager* DownloadManager::getTorrent() {
    if (!torrent_) {
        util::logLine("download: init torrent manager");
        torrent_.reset(new torrent::TorrentManager());
    }

    torrent_->setServerUrl(ds_manager_.remoteUrl());
    return torrent_.get();
}

void DownloadManager::ensureStreamConsumer(DownloadItem& item) {
    if (!item.preload_started || item.preload_file_index < 0 || item.torrent_id < 0) return;
    if (item.stream_consumer_started) return;

    std::string hash = item.torrent_hash;
    if (hash.empty() && torrent_) {
        torrent_->getTorrentHash(item.torrent_id, hash);
    }
    if (hash.empty()) return;

    const bool local_engine = (isLocalBackend());
    const std::string base_url = ensureHttpUrl(
        (torrent_ != nullptr && !torrent_->getServerUrl().empty())
            ? torrent_->getServerUrl()
            : ds_manager_.remoteUrl());
    const std::string route_name = streamRouteNameFromPath(item.preload_stream_name, hash);
    const int torrent_id = item.torrent_id;
    const int file_index = item.preload_file_index;

    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        for (const auto& existing : stream_consumers_) {
            if (existing.torrent_id == torrent_id) {
                item.stream_consumer_started = true;
                return;
            }
        }
    }

    auto stop_flag = std::make_shared<std::atomic<bool>>(false);
    StreamConsumer consumer;
    consumer.torrent_id = torrent_id;
    consumer.stop = stop_flag;
    consumer.worker = std::thread([stop_flag, base_url, route_name, hash, file_index, local_engine]() {
        net::HttpClient http;
        net::HttpClient preload_http;
        http.setKeepAlive(true);
        const int timeout_sec = local_engine ? 20 : kPumpTimeoutSec;
        const uint64_t chunk_bytes = local_engine ? (512ull * 1024ull) : kPumpChunkBytes;
        const uint64_t start_offset = local_engine ? 0ull : kPumpStartOffset;
        const int repreload_every = local_engine ? 4 : kPumpRepreloadEveryZeroReads;
        http.setTimeout(timeout_sec);
        preload_http.setTimeout(3);

        const std::string play_url = buildStreamPlayUrl(base_url, route_name, hash, file_index);
        const std::string preload_url = buildStreamPreloadUrl(base_url, route_name, hash, file_index);

        uint64_t offset = start_offset;
        int zero_reads = 0;
        bool logged_first_success = false;

        while (!stop_flag->load()) {
            size_t received = 0;
            int code = http.httpGetStream(play_url, offset, chunk_bytes,
                                          [&](const void*, size_t n) -> size_t {
                                              if (stop_flag->load()) return 0;
                                              received += n;
                                              return n;
                                          });
            if ((code == 200 || code == 206 || code == 0) && received > 0) {
                if (!logged_first_success) {
                    util::logLine("download: stream consumer received " + std::to_string(received) +
                                  " bytes from " + hash + " at offset=" + std::to_string(offset));
                    logged_first_success = true;
                }
                offset += static_cast<uint64_t>(received);
                zero_reads = 0;
                continue;
            }

            ++zero_reads;
            if ((zero_reads % repreload_every) == 0) {
                preload_http.httpGet(preload_url);
            }
            if (zero_reads == 1 || (zero_reads % 8) == 0) {
                util::logLine("download: stream consumer waiting hash=" + hash +
                              " offset=" + std::to_string(offset) +
                              " code=" + std::to_string(code) +
                              " zero_reads=" + std::to_string(zero_reads));
            }
            if ((zero_reads % kPumpRewindEveryZeroReads) == 0) {
                if (offset > kPumpRewindBytes) {
                    offset -= kPumpRewindBytes;
                } else {
                    offset = start_offset;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIdleSleepMs));
        }
    });

    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        stream_consumers_.push_back(std::move(consumer));
    }

    item.stream_consumer_started = true;
    util::logLine("download: stream consumer started hash=" + hash +
                  " file_index=" + std::to_string(file_index) +
                  " base_url=" + base_url +
                  (local_engine ? " mode=local" : " mode=remote"));
}

void DownloadManager::stopStreamConsumer(int torrent_id) {
    if (torrent_id < 0) return;

    std::thread worker;
    std::shared_ptr<std::atomic<bool>> stop_flag;

    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        for (auto it = stream_consumers_.begin(); it != stream_consumers_.end(); ++it) {
            if (it->torrent_id != torrent_id) continue;
            stop_flag = it->stop;
            worker = std::move(it->worker);
            stream_consumers_.erase(it);
            break;
        }
    }

    if (stop_flag) {
        stop_flag->store(true);
    }
    if (worker.joinable()) {
        worker.join();
    }
    if (stop_flag) {
        util::logLine("download: stream consumer stopped torrent_id=" + std::to_string(torrent_id));
    }
}

void DownloadManager::stopAllStreamConsumers() {
    std::vector<std::thread> workers;
    workers.reserve(stream_consumers_.size());
    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        for (auto& consumer : stream_consumers_) {
            if (consumer.stop) {
                consumer.stop->store(true);
            }
            if (consumer.worker.joinable()) {
                workers.push_back(std::move(consumer.worker));
            }
        }
        stream_consumers_.clear();
    }
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    for (auto& item : queue_) {
        item.stream_consumer_started = false;
    }
}

void DownloadManager::handleFileDownload(size_t index,
                                         const std::vector<torrent::TorrentInfo>& list,
                                         std::chrono::steady_clock::time_point now) {
    if (index >= queue_.size()) return;
    auto& item = queue_[index];
    if (item.state == DownloadState::Paused || item.state == DownloadState::Failed) return;

    // Живая сетевая статистика из списка торрентов движка (тот же сглаживатель,
    // что и в обычном пути), чтобы UI показывал скорость/пиров и для файла.
    if (!item.torrent_hash.empty()) {
        for (const auto& t : list) {
            if (!equalsIgnoreCase(item.torrent_hash, t.hash)) continue;
            item.seeds = t.seeds;
            item.peers = t.peers;
            item.known_peers = t.known_peers;
            item.dht = t.dht;
            if (t.download_speed_kbps >= 0.0f) {
                item.download_speed_kbps = smoothDownloadSpeedKbps(item.download_speed_kbps,
                                                                   t.download_speed_kbps,
                                                                   item.speed_sample_at, now);
            }
            item.speed_sample_at = now;
            break;
        }
    }

    if (!item.file_dl_dispatched) {
        auto* source = ds_manager_.getSource();
        if (!source) {
            item.state = DownloadState::Failed;
            item.error_message = "Data source is unavailable";
            return;
        }
        if (item.cancel_flag) item.cancel_flag->store(false);
        if (item.file_dl_cancel) item.file_dl_cancel->store(false);
        if (item.file_dl_pause) item.file_dl_pause->store(false);

        source->setTorrentContext(item.torrent_hash, normalizeTorrentLink(item.magnet), "");
        auto cancel = item.file_dl_cancel;
        auto pause = item.file_dl_pause;
        auto st = item.file_dl_state = std::make_shared<FileDownloadState>();
        const std::string hash = item.torrent_hash;
        const std::string magnet = normalizeTorrentLink(item.magnet);
        const std::string title = item.title;
        const int idx = item.forced_file_index;
        const auto sel_files = item.selected_files;
        const bool is_hb = item.is_homebrew;
        const std::string retro_cid = item.retro_console_id;
        const std::string title_id = item.title_id;
        const std::string cover_url = item.cover_url;

        item.file_dl_worker = std::make_shared<std::future<void>>(
            std::async(std::launch::async, [source, hash, magnet, title, title_id, cover_url, idx, sel_files, is_hb, retro_cid, cancel, pause, st]() {
                fileCopyWorker(source, hash, magnet, title, title_id, cover_url, idx, sel_files, is_hb, retro_cid, cancel, pause, st);
            }));
        item.file_dl_dispatched = true;
        util::logLine("download: file download started hash=" + hash +
                      " title='" + title + "' index=" + std::to_string(idx) +
                      " is_homebrew=" + (is_hb ? "true" : "false") +
                      " retro=" + retro_cid);
        return;
    }

    if (!item.file_dl_worker ||
        item.file_dl_worker->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        // Воркер ещё копирует: показываем прогресс.
        if (item.file_dl_state) {
            item.install_written = item.file_dl_state->written.load();
            item.install_total = item.file_dl_state->total.load();
            item.bytes_downloaded = item.install_written;
            item.download_total_bytes = item.install_total;
            if (item.install_total > 0) {
                item.progress = static_cast<float>(
                    (double)item.install_written / (double)item.install_total);
                item.install_progress = item.progress;
            }
            std::lock_guard<std::mutex> lk(item.file_dl_state->msg_mutex);
            if (!item.file_dl_state->stage_label.empty()) {
                item.install_stage_label = item.file_dl_state->stage_label;
            }
        }
        return;
    }

    item.file_dl_worker->get();
    item.file_dl_worker.reset();
    item.file_dl_dispatched = false;

    if (item.file_dl_state && item.file_dl_state->failed.load()) {
        item.state = DownloadState::Failed;
        item.error_message = item.file_dl_state->error.empty()
            ? "Failed to download file" : item.file_dl_state->error;
        if (!item.file_dl_state->dest.empty()) {
            std::error_code ec;
            std::filesystem::remove(item.file_dl_state->dest, ec);
        }
        item.install_written = item.file_dl_state->written.load();
        item.install_total = item.file_dl_state->total.load();
        item.bytes_downloaded = item.install_written;
        item.download_total_bytes = item.install_total;
        if (item.finish_time.time_since_epoch().count() == 0) {
            item.finish_time = std::chrono::steady_clock::now();
            if (item.start_time.time_since_epoch().count() > 0) {
                auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                    item.finish_time - item.start_time).count();
                if (sec > 0) item.elapsed_seconds += sec;
            }
            if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
        }
        releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
        util::logLine("download: file download failed: " + item.error_message);
        return;
    }

    item.state = DownloadState::Completed;
    item.progress = 1.0f;
    item.install_progress = 1.0f;
    if (item.finish_time.time_since_epoch().count() == 0) {
        item.finish_time = std::chrono::steady_clock::now();
        if (item.start_time.time_since_epoch().count() > 0) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                item.finish_time - item.start_time).count();
            if (sec > 0) item.elapsed_seconds += sec;
        }
        if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
    }
    if (item.file_dl_state) {
        item.install_total = item.file_dl_state->total.load();
        item.install_written = item.install_total;
        item.download_total_bytes = item.install_total;
        item.bytes_downloaded = item.install_total;
        if (!item.file_dl_state->dest.empty()) item.file_dl_dest = item.file_dl_state->dest;
        std::lock_guard<std::mutex> lk(item.file_dl_state->msg_mutex);
        item.retro_result_key = item.file_dl_state->result_key;
    }
    releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
    checkAndNotifyReleaseComplete(queue_, item);
}

void DownloadManager::handlePortAutoInstall(size_t index,
                                            const std::vector<torrent::TorrentInfo>& list,
                                            std::chrono::steady_clock::time_point now) {
    if (index >= queue_.size()) return;
    auto& item = queue_[index];
    if (item.state == DownloadState::Paused || item.state == DownloadState::Failed) return;

    if (!item.torrent_hash.empty()) {
        for (const auto& t : list) {
            if (!equalsIgnoreCase(item.torrent_hash, t.hash)) continue;
            item.seeds = t.seeds;
            item.peers = t.peers;
            item.known_peers = t.known_peers;
            item.dht = t.dht;
            if (t.download_speed_kbps >= 0.0f) {
                item.download_speed_kbps = smoothDownloadSpeedKbps(item.download_speed_kbps,
                                                                   t.download_speed_kbps,
                                                                   item.speed_sample_at, now);
            }
            item.speed_sample_at = now;
            break;
        }
    }

    if (!item.port_install_dispatched) {
        // A cancelled worker of the same torrent still holds the stream: wait for it.
        reapRetiredWorkers();
        if (hasRetiredWorker(item.torrent_hash)) return;
        auto* source = ds_manager_.getSource();
        if (!source) {
            item.state = DownloadState::Failed;
            item.error_message = "app/ports/error_engine"_i18n;
            return;
        }
        if (item.cancel_flag) item.cancel_flag->store(false);
        if (item.port_install_cancel) item.port_install_cancel->store(false);
        if (item.port_install_pause) item.port_install_pause->store(false);

        source->setTorrentContext(item.torrent_hash, normalizeTorrentLink(item.magnet), "");
        auto cancel = item.port_install_cancel;
        auto pause = item.port_install_pause;
        auto st = item.port_install_state = std::make_shared<PortInstallProgressState>();
        const std::string hash = item.torrent_hash;
        const std::string magnet = normalizeTorrentLink(item.magnet);
        const std::string title = item.title;
        const std::string topic_id = item.topic_id;
        auto plan = item.port_plan;
        const int variant_idx = item.selected_variant_index;
        const bool with_optional = item.port_include_optional;
        if (!item.port_resume) item.port_resume = std::make_shared<PortInstallResume>();
        auto resume = item.port_resume;

        item.port_install_worker = std::make_shared<std::future<void>>(
            std::async(std::launch::async, [source, hash, magnet, title, topic_id, plan, variant_idx, with_optional, cancel, pause, st, resume]() {
                portInstallWorker(source, hash, magnet, title, topic_id, plan, variant_idx, with_optional, cancel, pause, st, resume);
            }));
        item.port_install_dispatched = true;
        util::logLine("download: port auto install started hash=" + hash +
                      " title='" + title + "' variant=" + std::to_string(variant_idx));
        return;
    }

    if (!item.port_install_worker ||
        item.port_install_worker->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (item.port_install_state) {
            auto st = item.port_install_state;
            item.install_written = st->inst_written.load();
            item.install_total = st->inst_total.load();
            item.install_progress = st->inst_progress.load();

            item.bytes_downloaded = st->dl_written.load();
            item.download_total_bytes = st->dl_total.load();
            item.progress = st->dl_progress.load();

            if (item.install_total == 0) {
                item.install_total = item.download_total_bytes;
            }

            std::lock_guard<std::mutex> lk(st->msg_mutex);
            const int rv = st->resolved_variant.load();
            if (rv >= 0 && rv != item.selected_variant_index) {
                item.selected_variant_index = rv;
                if (item.port_plan) item.selected_files = item.port_plan->getFilesForVariant(rv, item.port_include_optional);
            }
            if (!item.port_plan && st->resolved_plan) {
                item.port_plan = st->resolved_plan;
                item.selected_files = item.port_plan->getFilesForVariant(item.selected_variant_index, item.port_include_optional);
            }
            if (!st->stage_label.empty()) {
                item.install_stage_label = st->stage_label;
            }
            if (!st->current_file.empty()) {
                item.install_current_file = st->current_file;
            }
        }
        return;
    }

    item.port_install_worker->get();
    item.port_install_worker.reset();
    item.port_install_dispatched = false;

    if (item.port_install_state && item.port_install_state->failed.load()) {
        item.state = DownloadState::Failed;
        item.error_message = item.port_install_state->error.empty()
            ? "app/ports/error_failed"_i18n : item.port_install_state->error;
        if (item.finish_time.time_since_epoch().count() == 0) {
            item.finish_time = std::chrono::steady_clock::now();
            if (item.start_time.time_since_epoch().count() > 0) {
                auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                    item.finish_time - item.start_time).count();
                if (sec > 0) item.elapsed_seconds += sec;
            }
            if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
        }
        releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
        util::logLine("download: port install failed: " + item.error_message);
        return;
    }

    item.state = DownloadState::Completed;
    item.progress = 1.0f;
    item.install_progress = 1.0f;
    if (item.download_total_bytes > 0) item.bytes_downloaded = item.download_total_bytes;
    if (item.install_total > 0) item.install_written = item.install_total;
    item.install_stage_label = "app/ports/stage_done"_i18n;
    if (item.port_install_state) {
        std::string note;
        {
            std::lock_guard<std::mutex> lk(item.port_install_state->msg_mutex);
            note = item.port_install_state->result_note;
        }
        if (!note.empty()) {
            item.install_current_file = note;
            const std::string title = cleanTitle(item.title);
            brls::sync([title, note]() { brls::Application::notify(title + ": " + note); });
        }
    }
    if (item.finish_time.time_since_epoch().count() == 0) {
        item.finish_time = std::chrono::steady_clock::now();
        if (item.start_time.time_since_epoch().count() > 0) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                item.finish_time - item.start_time).count();
            if (sec > 0) item.elapsed_seconds += sec;
        }
        if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
    }
    releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
    checkAndNotifyReleaseComplete(queue_, item);
}

bool DownloadManager::startDownload(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state != DownloadState::Queued) return false;

    if (item.stream_consumer_started && item.torrent_id >= 0) {
        stopStreamConsumer(item.torrent_id);
        item.stream_consumer_started = false;
    }

    if (item.magnet.empty()) {
        item.state = DownloadState::Failed;
        item.error_message = "Empty torrent link";
        util::logLine("download: empty torrent link for " + item.title);
        return false;
    }

    const std::string link = normalizeTorrentLink(item.magnet);
    const bool local_mode_only = (isLocalBackend());
    std::string local_hash;
    int id = -1;
    if (local_mode_only) {
        local_hash = extractBtihHash(link);
        if (local_hash.empty()) {
            item.state = DownloadState::Failed;
            item.error_message = "Failed to resolve BTIH hash from magnet";
            util::logLine("download: local client cannot start without BTIH hash for " + item.title);
            return false;
        }
        // Adopt the probe's kept torrent (if any) so the file-select cleanup
        // does not remove it from under the download.
        datasource::CustomEngineClient::instance().markInUse(local_hash);
        util::logLine("download: local client queued hash=" + local_hash);
    } else {
        id = getTorrent()->addMagnet(link);
        if (id < 0) {
            item.state = DownloadState::Failed;
            item.error_message = "Failed to add torrent to TorrServer";
            util::logLine("download: failed to add torrent to TorrServer for " + item.title);
            return false;
        }
    }

    item.state = DownloadState::Downloading;
    item.torrent_id = id;
    item.preload_started = false;
    item.preload_file_index = -1;
    item.preload_stream_name.clear();
    item.auto_hybrid_started = false;
    item.stream_consumer_started = false;
    item.hybrid_installer.reset();
    item.pump_offset = 0;
    item.pump_zero_reads = 0;
    item.pump_last_at = std::chrono::steady_clock::time_point{};
    item.download_speed_kbps = 0.0f;
    item.speed_sample_at = std::chrono::steady_clock::time_point{};
    item.start_time = std::chrono::steady_clock::now();
    item.finish_time = std::chrono::steady_clock::time_point{};
    item.elapsed_seconds = 0;
    item.error_message.clear();
    item.torrent_hash.clear();

    item.file_dl_dispatched = false;
    item.file_dl_worker.reset();
    if (item.file_dl_cancel) item.file_dl_cancel->store(false);
    item.file_dl_state.reset();
    item.file_dl_dest.clear();

    if (local_mode_only) {
        item.torrent_hash = local_hash;
    } else if (item.torrent_id >= 0) {
        std::string hash;
        if (torrent_ && torrent_->getTorrentHash(item.torrent_id, hash)) {
            item.torrent_hash = hash;
            util::logLine("download: torrent hash: " + hash);
        }
    }

    if (item.torrent_hash.empty()) {
        item.torrent_hash = extractBtihHash(link);
        if (item.torrent_hash.empty()) {
            item.state = DownloadState::Failed;
            item.error_message = "Failed to resolve BTIH hash from magnet";
            util::logLine("download: addMagnet failed and BTIH is unavailable for " + item.title);
            return false;
        }
        if (local_mode_only) {
            util::logLine("download: local client hash=" + item.torrent_hash);
        } else {
            util::logLine("download: TorrServer add failed, using local client hash=" + item.torrent_hash);
        }
    }

    return true;
}

void DownloadManager::startNextDownload() {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (queue_[i].state != DownloadState::Queued) continue;
        if (startDownload(i)) return;
    }
}

void DownloadManager::trackProgress() {
    // Poll the engine's torrent list BEFORE taking the queue mutex: the UI
    // thread takes the same mutex, and nothing here needs the queue. The list
    // and its timestamp are only ever touched by this (progress) thread.
    bool list_refreshed = false;
    if (isLocalBackend() &&
        (ds_manager_.mode() == datasource::DataSourceMode::CustomEngine ||
         ds_manager_.mode() == datasource::DataSourceMode::LocalClient)) {
        const auto now = std::chrono::steady_clock::now();
        if (last_torrent_list_poll_.time_since_epoch().count() == 0 ||
            (now - last_torrent_list_poll_) >= std::chrono::milliseconds(800)) {
            std::vector<torrent::TorrentInfo> fresh_list;
            customEngineGetTorrentList(fresh_list);
            last_torrent_list_ = std::move(fresh_list);
            last_torrent_list_poll_ = now;
            list_refreshed = true;
        }
    }

    std::vector<std::string> deferred_releases;
    {
        DeferredReleaseScope release_scope(deferred_releases);
        const uint64_t lock_wait_start = perf::nowUs();
        std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
        perf::LockTimer lock_timer(perf::LockSite::TrackProgress, lock_wait_start);
        trackProgressLocked(list_refreshed);
    }
    // Torrent teardown (joins engine threads) with the queue mutex released.
    runDeferredReleases(deferred_releases);
}

void DownloadManager::trackProgressLocked(bool list_refreshed) {
    auto now = std::chrono::steady_clock::now();
    bool refreshed_torrent_list = false;
    const bool is_local_client = (isLocalBackend());
    if (torrent_ && !is_local_client) {
        const auto poll_interval = std::chrono::milliseconds(800);
        if (last_torrent_list_.empty() ||
            last_torrent_list_poll_.time_since_epoch().count() == 0 ||
            (now - last_torrent_list_poll_) >= poll_interval) {
            auto fresh_list = torrent_->getTorrentList();
            if (!fresh_list.empty()) {
                last_torrent_list_ = std::move(fresh_list);
            }
            last_torrent_list_poll_ = now;
            refreshed_torrent_list = true;
        }
    } else if (is_local_client) {
        const auto poll_interval = std::chrono::milliseconds(800);
        if (list_refreshed) {
            refreshed_torrent_list = true;   // polled by trackProgress() without the lock
        } else if (last_torrent_list_poll_.time_since_epoch().count() == 0 ||
            (now - last_torrent_list_poll_) >= poll_interval) {
            std::vector<torrent::TorrentInfo> fresh_list;
            if (ds_manager_.mode() == datasource::DataSourceMode::CustomEngine ||
                ds_manager_.mode() == datasource::DataSourceMode::LocalClient) {
                customEngineGetTorrentList(fresh_list);
            } else if (torrent_) {
                fresh_list = torrent_->getTorrentList();
            }
            last_torrent_list_ = std::move(fresh_list);
            last_torrent_list_poll_ = now;
            refreshed_torrent_list = true;
        }
    }

    const auto& list = last_torrent_list_;
    for (size_t i = 0; i < queue_.size(); ++i) {
        auto& item = queue_[i];
        const bool downloading_active =
            (item.state == DownloadState::Downloading || item.state == DownloadState::StreamInstalling);
        float hybrid_speed_kbps = -1.0f;

        if (!downloading_active && item.stream_consumer_started && item.torrent_id >= 0) {
            stopStreamConsumer(item.torrent_id);
            item.stream_consumer_started = false;
        }

        if (item.state == DownloadState::Cancelled ||
            item.state == DownloadState::Completed ||
            item.state == DownloadState::Failed) {
            continue;
        }

        if (item.hybrid_installer && downloading_active) {
            float install_p = item.hybrid_installer->progress();
            if (!(install_p >= 0.0f)) install_p = 0.0f;
            if (install_p > 1.0f) install_p = 1.0f;
            item.install_progress = install_p;

            float dl_p = item.hybrid_installer->downloadProgress();
            if (!(dl_p >= 0.0f)) dl_p = 0.0f;
            if (dl_p > 1.0f) dl_p = 1.0f;
            item.progress = dl_p;
            hybrid_speed_kbps = static_cast<float>(item.hybrid_installer->downloadSpeedKbps());
            if (!(hybrid_speed_kbps >= 0.0f)) hybrid_speed_kbps = 0.0f;
            item.install_written = item.hybrid_installer->bytesInstalled();
            item.install_total = item.hybrid_installer->totalBytes();
            uint64_t cur_dl_bytes = item.hybrid_installer->bytesDownloaded();
            uint64_t cur_dl_total = item.hybrid_installer->downloadTotalBytes();
            if (cur_dl_bytes > 0) item.bytes_downloaded = cur_dl_bytes;
            if (cur_dl_total > 0) item.download_total_bytes = cur_dl_total;

            if (item.hybrid_installer->hasSdkMismatch()) {
                item.sdk_mismatch = true;
                item.game_sdk = item.hybrid_installer->gameSdk();
                item.console_sdk = item.hybrid_installer->consoleSdk();
            }

            if (item.hybrid_installer->isFinished()) {
                if (item.hybrid_installer->hasError()) {
                    item.state = DownloadState::Failed;
                    item.error_message = item.hybrid_installer->errorMessage();
                    item.download_speed_kbps = 0.0f;
                    item.speed_sample_at = std::chrono::steady_clock::time_point{};
                    if (item.finish_time.time_since_epoch().count() == 0) {
                        item.finish_time = std::chrono::steady_clock::now();
                        if (item.start_time.time_since_epoch().count() > 0) {
                            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                                item.finish_time - item.start_time).count();
                            if (sec > 0) item.elapsed_seconds += sec;
                        }
                        if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
                    }
                    if (item.torrent_id >= 0) {
                        torrent_->cancelTorrent(item.torrent_id);
                    }
                    if (!item.torrent_hash.empty()) {
                        releaseTorrentIfUnneeded(queue_, i, item.torrent_hash);
                    }
                    util::logLine("download: hybrid install failed: " + item.error_message);
                } else {
                    item.state = DownloadState::Completed;
                    item.progress = 1.0f;
                    item.install_progress = 1.0f;
                    if (item.hybrid_installer) {
                        uint64_t dl_bytes = item.hybrid_installer->bytesDownloaded();
                        uint64_t dl_total = item.hybrid_installer->downloadTotalBytes();
                        uint64_t inst_total = item.hybrid_installer->totalBytes();
                        if (dl_bytes > 0) item.bytes_downloaded = dl_bytes;
                        if (dl_total > 0) item.download_total_bytes = dl_total;
                        else if (item.download_total_bytes == 0 && inst_total > 0) item.download_total_bytes = inst_total;
                        if (item.bytes_downloaded == 0 && item.download_total_bytes > 0) item.bytes_downloaded = item.download_total_bytes;
                        if (item.install_total == 0) item.install_total = (inst_total > 0) ? inst_total : item.download_total_bytes;
                    }
                    if (item.install_total == 0) {
                        item.install_total = item.download_total_bytes;
                    }
                    item.install_written = item.install_total;
                    copyDownloadedOtherFiles(item);
                    item.download_speed_kbps = 0.0f;
                    item.speed_sample_at = std::chrono::steady_clock::time_point{};
                    if (item.finish_time.time_since_epoch().count() == 0) {
                        item.finish_time = std::chrono::steady_clock::now();
                        if (item.start_time.time_since_epoch().count() > 0) {
                            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                                item.finish_time - item.start_time).count();
                            if (sec > 0) item.elapsed_seconds += sec;
                        }
                        if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
                    }
                    if (item.torrent_id >= 0) {
                        torrent_->cancelTorrent(item.torrent_id);
                    }
                    if (!item.torrent_hash.empty()) {
                        releaseTorrentIfUnneeded(queue_, i, item.torrent_hash);
                    }
                    util::logLine("download: hybrid install completed: " + item.title);
                    checkAndNotifyReleaseComplete(queue_, item);
                }
                item.hybrid_installer.reset();
                if (item.stream_consumer_started && item.torrent_id >= 0) {
                    stopStreamConsumer(item.torrent_id);
                    item.stream_consumer_started = false;
                }
                continue;
            }
        }

        if (item.state == DownloadState::StreamPreparing) {
            startHybridInstall(i);
            continue;
        }

        // Homebrew ports: multi-phase auto install pipeline
        if (isLocalBackend() &&
            item.state == DownloadState::Downloading &&
            item.is_port_auto_install) {
            handlePortAutoInstall(i, list, now);
            continue;
        }

        // Homebrew/ports, retro ROMs, or non-game files: download directly to downloads/ or retro folder without installation
        if (isLocalBackend() &&
            item.state == DownloadState::Downloading &&
            (item.is_homebrew ||
             !item.retro_console_id.empty() ||
             (item.forced_file_index >= 0 && !isSwitchGameFile(item.forced_stream_name)) ||
             (!item.selected_files.empty() && !isSwitchGameFile(item.forced_stream_name)))) {
            handleFileDownload(i, list, now);
            continue;
        }

        if (isLocalBackend() &&
            item.state == DownloadState::Downloading &&
            !item.is_homebrew &&
            item.retro_console_id.empty() &&
            item.forced_file_index >= 0 &&
            isSwitchGameFile(item.forced_stream_name) &&
            !item.auto_hybrid_started &&
            !item.hybrid_installer) {
            item.preload_started = true;
            item.preload_file_index = item.forced_file_index;
            item.preload_stream_name = item.forced_stream_name;
            util::logLine("download: local client starts selected file without HTTP preload hash=" +
                          item.torrent_hash +
                          " file_index=" + std::to_string(item.preload_file_index));
            startHybridInstall(i);
            continue;
        }

        if (item.state == DownloadState::Downloading ||
            item.state == DownloadState::StreamInstalling ||
            item.state == DownloadState::StreamPreparing ||
            item.state == DownloadState::Paused) {
            if (!item.priorities_set && !item.selected_files.empty()) {
                std::vector<torrent::TorrentFileInfo> files;
                bool got_files = false;
                if (item.torrent_id >= 0 && torrent_) {
                    got_files = torrent_->getTorrentFiles(item.torrent_id, files);
                } else if (!item.torrent_hash.empty() && isLocalBackend()) {
                    got_files = customEngineGetFiles(item.torrent_hash, files);
                }

                if (got_files && !files.empty()) {
                    for (const auto& f : files) {
                        bool wanted = std::find(item.selected_files.begin(), item.selected_files.end(), f.index) != item.selected_files.end();
                        if (item.torrent_id >= 0 && torrent_) {
                            setFileWanted(i, f.index, wanted);
                        } else if (!item.torrent_hash.empty() && isLocalBackend()) {
                            customEngineSetFileWanted(item.torrent_hash, f.index, wanted);
                        }

                        if (wanted && f.index == item.forced_file_index) {
                            item.install_total = f.size;
                        }
                    }
                    item.priorities_set = true;
                    util::logLine("download: configured file priorities and size=" + std::to_string(item.install_total) + " for " + item.title);
                }
            }
            bool matched = false;
            for (const auto& t : list) {
                bool match = false;
                if (item.torrent_id >= 0 && t.id == item.torrent_id) {
                    match = true;
                } else if (!item.torrent_hash.empty() && !t.hash.empty() &&
                           equalsIgnoreCase(item.torrent_hash, t.hash)) {
                    match = true;
                } else if (item.torrent_id < 0 && !item.title.empty() && t.name == item.title) {
                    match = true;
                }

                if (!match) {
                    continue;
                }

                matched = true;
                item.seeds = t.seeds;
                item.peers = t.peers;
                item.known_peers = t.known_peers;
                item.dht = t.dht;

                // Self-heal a pause that raced the stream open: the engine
                // torrent may have landed after pause_torrent() ran.
                if (item.state == DownloadState::Paused &&
                    !item.torrent_hash.empty()) {
                    tsnx_engine_pause_torrent(nullptr, item.torrent_hash.c_str());
                }

                // Only the actively transferring item takes the torrent's
                // progress/speed: several queued items can share the same
                // info-hash (base game + DLC), and they would otherwise all
                // display the running torrent's numbers.
                const bool active_transfer =
                    item.state == DownloadState::Downloading ||
                    item.state == DownloadState::StreamInstalling ||
                    item.state == DownloadState::StreamPreparing;

                if (active_transfer) {
                    // If hybrid installer is active, its downloadProgress() is already the overall progress.
                    // Otherwise, use the engine's per-torrent completion.
                    if (!item.hybrid_installer) {
                        item.progress = t.percent_done;
                    }

                    float sampled_speed_kbps = t.download_speed_kbps;
                    if (hybrid_speed_kbps >= 0.0f) {
                        const bool local_hybrid_stream =
                            isLocalBackend() &&
                            item.state == DownloadState::StreamInstalling &&
                            item.hybrid_installer != nullptr;

                        // In LocalClient mode, prefer the engine's actual network speed
                        // (t.download_speed_kbps) over the installer's internal reading speed
                        // (hybrid_speed_kbps), as the latter can drop to 0 when the ring buffer
                        // is full even if the download is active.
                        sampled_speed_kbps = local_hybrid_stream
                            ? t.download_speed_kbps
                            : std::max(sampled_speed_kbps, hybrid_speed_kbps);
                    }
                    if (refreshed_torrent_list ||
                        hybrid_speed_kbps >= 0.0f ||
                        item.speed_sample_at.time_since_epoch().count() == 0) {
                        item.download_speed_kbps = smoothDownloadSpeedKbps(item.download_speed_kbps,
                                                                           sampled_speed_kbps,
                                                                           item.speed_sample_at,
                                                                           now);
                        item.speed_sample_at = now;
                    }
                }
                if (item.torrent_hash.empty() && !t.hash.empty()) {
                    item.torrent_hash = t.hash;
                }

                if (!item.preload_started && item.torrent_id >= 0) {
                    int chosen_index = -1;
                    std::string chosen_stream_name;

                    if (item.forced_file_index >= 0) {
                        chosen_index = item.forced_file_index;
                        chosen_stream_name = item.forced_stream_name;
                    } else {
                        std::vector<torrent::TorrentFileInfo> files;
                        if (torrent_->getTorrentFiles(item.torrent_id, files) && !files.empty()) {
                            chooseInstallFile(files, chosen_index, chosen_stream_name);
                        }
                    }

                    if (chosen_index >= 0 &&
                        torrent_->preloadTorrentFile(item.torrent_id, chosen_index, chosen_stream_name)) {
                        item.preload_started = true;
                        item.preload_file_index = chosen_index;
                        item.preload_stream_name = chosen_stream_name;
                        util::logLine("download: preload started hash=" + item.torrent_hash +
                                      " file_index=" + std::to_string(chosen_index));
                    }
                }

                if (item.preload_started) {
                    if (item.state == DownloadState::Downloading &&
                        !item.auto_hybrid_started &&
                        !item.hybrid_installer &&
                        isSwitchGameFile(item.preload_stream_name)) {
                        if (startHybridInstall(i)) {
                            item.auto_hybrid_started = true;
                        }
                    }
                    if (item.state == DownloadState::Downloading &&
                        !item.hybrid_installer &&
                        isSwitchGameFile(item.preload_stream_name) &&
                        !isLocalBackend()) {
                        ensureStreamConsumer(item);
                    }
                }

                if (!item.hybrid_installer && !item.preload_started &&
                    !item.stream_ready && item.torrent_id >= 0) {
                    std::vector<std::string> paths;
                    std::string name;
                    if (torrent_->getStreamFiles(item.torrent_id, paths, name)) {
                        item.stream_ready = true;
                        item.stream_name = name;
                        item.stream_files.clear();
                        for (const auto& p : paths) {
                            DownloadItem::StreamFile sf;
                            sf.path = p;
                            sf.offset = 0;
                            item.stream_files.push_back(sf);
                        }
                        item.stream_index = 0;
                        item.installer.openStream(item.stream_name);
                        item.install_total = item.installer.totalSize();
                        item.install_written = item.installer.writtenSize();
                    }
                }

                if (!item.hybrid_installer && !item.preload_started &&
                    item.stream_ready && !item.stream_done) {
                    size_t max_bytes = 1024 * 1024;
                    if (item.stream_index < item.stream_files.size()) {
                        auto& sf = item.stream_files[item.stream_index];
                        uint64_t fsize = 0;
                        if (getFileSize(sf.path, fsize) && fsize > sf.offset) {
                            readChunkToInstaller(sf.path, sf.offset, item.installer, max_bytes);
                            item.installer.installChunk();
                            item.install_total = item.installer.totalSize();
                            item.install_written = item.installer.writtenSize();
                            if (item.install_total > 0) {
                                item.install_progress = (float)((double)item.install_written / (double)item.install_total);
                            }
                        } else if (getFileSize(sf.path, fsize) && fsize == sf.offset) {
                            if (item.progress >= 1.0f || fsize > 0) {
                                item.stream_index++;
                            }
                        }
                    }
                    if (item.installer.isComplete()) {
                        item.installer.closeStream();
                        item.stream_done = true;
                        item.state = DownloadState::Installing;
                    }
                }

                if (!item.hybrid_installer && !item.preload_started &&
                    item.progress >= 1.0f && !item.stream_ready) {
                    item.state = DownloadState::Installing;
                }
                break;
            }

            if (!matched && item.preload_started &&
                item.state == DownloadState::Downloading &&
                !item.hybrid_installer) {
                ensureStreamConsumer(item);
            }

            if (!matched && hybrid_speed_kbps >= 0.0f) {
                item.download_speed_kbps = smoothDownloadSpeedKbps(item.download_speed_kbps,
                                                                   hybrid_speed_kbps,
                                                                   item.speed_sample_at,
                                                                   now);
                item.speed_sample_at = now;
            }

            if (!matched &&
                item.state == DownloadState::Downloading &&
                !item.is_homebrew &&
                item.retro_console_id.empty() &&
                !item.auto_hybrid_started &&
                !item.hybrid_installer &&
                ds_manager_.mode() != datasource::DataSourceMode::LocalClient) {
                if (startHybridInstall(i)) {
                    item.auto_hybrid_started = true;
                }
            }
        } else if (item.state == DownloadState::Installing) {
            if (item.install_total > 0) {
                item.install_progress = (float)((double)item.install_written / (double)item.install_total);
            }
            if (item.stream_done || item.stream_ready || (!item.stream_ready && item.progress >= 1.0f)) {
                item.state = DownloadState::Completed;
                copyDownloadedOtherFiles(item);
                item.download_speed_kbps = 0.0f;
                item.speed_sample_at = std::chrono::steady_clock::time_point{};
                if (item.finish_time.time_since_epoch().count() == 0) {
                    item.finish_time = std::chrono::steady_clock::now();
                    if (item.start_time.time_since_epoch().count() > 0) {
                        auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                            item.finish_time - item.start_time).count();
                        if (sec > 0) item.elapsed_seconds += sec;
                    }
                    if (item.elapsed_seconds <= 0) item.elapsed_seconds = 1;
                }
                if (item.stream_consumer_started && item.torrent_id >= 0) {
                    stopStreamConsumer(item.torrent_id);
                    item.stream_consumer_started = false;
                }
                if (!item.torrent_hash.empty() && isLocalBackend()) {
                    releaseTorrentIfUnneeded(queue_, i, item.torrent_hash);
                }
                checkAndNotifyReleaseComplete(queue_, item);
            }
        }

        // Calculate and smooth installation speed
        if (item.state == DownloadState::StreamInstalling || item.state == DownloadState::Installing) {
            unsigned long long current_install_written = item.install_written;
            if (item.install_speed_sample_at.time_since_epoch().count() == 0) {
                item.install_speed_sample_at = now;
                item.last_install_written = current_install_written;
                item.install_speed_kbps = 0.0f;
            } else {
                double dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - item.install_speed_sample_at).count() / 1000.0;
                if (dt >= 0.5) { // update speed sample every 500ms or more
                    unsigned long long delta = 0;
                    if (current_install_written >= item.last_install_written) {
                        delta = current_install_written - item.last_install_written;
                    }
                    float sampled_speed_kbps = static_cast<float>(delta) / 1024.0f / dt;
                    item.install_speed_kbps = smoothDownloadSpeedKbps(item.install_speed_kbps,
                                                                      sampled_speed_kbps,
                                                                      item.install_speed_sample_at,
                                                                      now);
                    item.install_speed_sample_at = now;
                    item.last_install_written = current_install_written;
                }
            }
        } else {
            item.install_speed_kbps = 0.0f;
            item.install_speed_sample_at = std::chrono::steady_clock::time_point{};
            item.last_install_written = 0;
        }
    }

    bool has_active = false;
    bool has_queued = false;
    bool has_paused = false;
    for (const auto& item : queue_) {
        if (isTransferActive(item.state)) {
            has_active = true;
        } else if (item.state == DownloadState::Queued) {
            has_queued = true;
        } else if (item.state == DownloadState::Paused) {
            has_paused = true;
        }
    }

    if (!has_active && !has_paused && has_queued) {
        startNextDownload();
        has_active = hasActiveTransfers();
    }

    updateSleepPolicy(has_active);

}

bool DownloadManager::hasActiveTransfers() const {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    for (const auto& item : queue_) {
        if (isTransferActive(item.state)) {
            return true;
        }
    }
    return false;
}

void DownloadManager::startLocalProbe(DownloadItem& item) {
    auto job = std::make_shared<DownloadItem::ProbeJob>();
    item.probe_job = job;
    item.probe_retry_at = std::chrono::steady_clock::time_point{};
    has_open_pending_.store(true);   // progress loop polls every 100 ms meanwhile
    util::logLine("download: background probeFiles started hash=" + item.torrent_hash);
    // Background: the job is shared, so dropping the item never waits for it.
    // CustomEngineClient::shutdown() waits for an in-flight probe itself.
    bool started = util::runInBackground([job, hash = item.torrent_hash, magnet = item.magnet]() {
        tsnx_thread_pin_self(TSNX_CORE_NET, TSNX_PRIO_NET_BG);
        std::vector<datasource::CustomEngineFileInfo> files;
        std::string err;
        bool ok = false;
        if (!g_appExiting.load()) {
            ok = datasource::CustomEngineClient::instance().probeFiles(hash, magnet, "", files, &err);
        } else {
            err = "app is exiting";
        }
        job->files = std::move(files);
        job->error = std::move(err);
        job->ok = ok;
        job->done.store(true, std::memory_order_release);
    }, "probe");
    if (!started) {
        // Finish the job as failed so the progress loop does not wait forever
        job->error = "could not start probe thread";
        job->ok = false;
        job->done.store(true, std::memory_order_release);
    }
}

bool DownloadManager::startHybridInstall(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.is_homebrew || !item.retro_console_id.empty()) {
        return false;
    }

    if (item.state == DownloadState::Queued) {
        if (!startDownload(index)) {
            return false;
        }
    }

    if (item.state != DownloadState::Downloading &&
        item.state != DownloadState::StreamPreparing &&
        item.state != DownloadState::StreamInstalling) {
        util::logLine("download: hybrid install cannot start in current state=" +
                      std::to_string(static_cast<int>(item.state)));
        return false;
    }

    if (item.hybrid_installer && !item.hybrid_installer->isFinished() &&
        item.state != DownloadState::StreamPreparing) {
        return true;
    }

    if (item.torrent_hash.empty()) {
        item.torrent_hash = extractBtihHash(normalizeTorrentLink(item.magnet));
    }
    if (item.torrent_hash.empty()) {
        util::logLine("download: hybrid install missing torrent hash");
        return false;
    }

    const bool local_client_mode = (isLocalBackend());
    const bool has_remote_torrent = (!local_client_mode &&
                                     item.torrent_id >= 0 &&
                                     torrent_ != nullptr &&
                                     torrent_->isServerReachable());

    int install_file_index = item.preload_file_index;
    std::string install_stream_name = item.preload_stream_name;
    if (install_file_index < 0) {
        if (item.forced_file_index >= 0) {
            install_file_index = item.forced_file_index;
            install_stream_name = item.forced_stream_name;
        } else {
            if (has_remote_torrent) {
                std::vector<torrent::TorrentFileInfo> files;
                if (!torrent_->getTorrentFiles(item.torrent_id, files) || files.empty()) {
                    return false;
                }
                if (!chooseInstallFile(files, install_file_index, install_stream_name)) {
                    util::logLine("download: no installable NSP/NSZ/XCI file found in torrent");
                    return false;
                }
            } else if (isLocalBackend()) {
                // probeFiles() fetches the metadata (announce + DHT, up to tens
                // of seconds). It runs on its own thread; until it is done this
                // returns false ("not started yet") and the progress loop calls
                // again on its next tick, without ever blocking the queue.
                if (!item.probe_job) {
                    if (item.probe_retry_at.time_since_epoch().count() != 0 &&
                        std::chrono::steady_clock::now() < item.probe_retry_at) {
                        return false;
                    }
                    startLocalProbe(item);
                    return false;
                }
                if (!item.probe_job->done.load(std::memory_order_acquire)) {
                    return false;   // still fetching
                }
                std::shared_ptr<DownloadItem::ProbeJob> job = std::move(item.probe_job);
                item.probe_job.reset();
                if (!job->ok || job->files.empty()) {
                    util::logLine("download: local probeFiles failed in hybrid start: " + job->error);
                    has_open_pending_.store(false);
                    item.probe_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    return false;
                }
                std::vector<datasource::CustomEngineFileInfo> local_files = std::move(job->files);
                std::vector<torrent::TorrentFileInfo> files;
                files.reserve(local_files.size());
                for (const auto& lf : local_files) {
                    torrent::TorrentFileInfo tf;
                    tf.index = lf.index;
                    tf.name = lf.path;
                    tf.size = lf.size;
                    tf.wanted = true;
                    files.push_back(std::move(tf));
                }
                if (!chooseInstallFile(files, install_file_index, install_stream_name)) {
                    util::logLine("download: no installable NSP/NSZ/XCI file found in local torrent");
                    return false;
                }
            } else {
                util::logLine("download: hybrid install requires an explicit file selection in local mode");
                return false;
            }
        }

        if (install_file_index < 0) {
            util::logLine("download: hybrid install invalid file index");
            return false;
        }
        item.preload_file_index = install_file_index;
        item.preload_stream_name = install_stream_name;
    }

    if (!item.preload_started) {
        if (has_remote_torrent) {
            if (torrent_->preloadTorrentFile(item.torrent_id, install_file_index, install_stream_name)) {
                item.preload_started = true;
                util::logLine("download: preload started for hybrid hash=" + item.torrent_hash +
                              " file_index=" + std::to_string(install_file_index));
            } else {
                util::logLine("download: hybrid install preload failed on remote, switching to internal stream");
                item.preload_started = true;
            }
        } else {
            item.preload_started = true;
            util::logLine("download: internal mode, preload skipped");
        }
    }

    if (item.stream_consumer_started && item.torrent_id >= 0) {
        stopStreamConsumer(item.torrent_id);
        item.stream_consumer_started = false;
    }

    auto* source = ds_manager_.getSource();
    if (!source) {
        util::logLine("download: data source is unavailable");
        return false;
    }

    if (item.state == DownloadState::Downloading) {
        if (!item.cancel_flag) {
            item.cancel_flag = std::make_shared<std::atomic<bool>>(false);
        }
        item.cancel_flag->store(false);
        source->setTorrentContext(item.torrent_hash, normalizeTorrentLink(item.magnet), "");

        item.state = DownloadState::StreamPreparing;
        util::logLine("download: starting async open for hybrid hash=" + item.torrent_hash +
                      " index=" + std::to_string(install_file_index));
        has_open_pending_.store(true);
        
        auto cancel_flag = item.cancel_flag;
        item.open_future = std::make_shared<std::future<bool>>(
            std::async(std::launch::async, [source, hash = item.torrent_hash, idx = install_file_index, cancel_flag]() {
                if (source) {
                    source->setSchedulerEnabled(true);
                    source->setCancelFlag(cancel_flag.get());
                }
                return source ? source->open(hash, idx) : false;
            })
        );
        return true;
    }

    if (item.state == DownloadState::StreamPreparing) {
        if (item.start_future) {
            if (!item.start_future->valid()) {
                item.start_future.reset();
                item.hybrid_installer.reset();
                item.state = DownloadState::Failed;
                item.error_message = "Installer start task invalid state";
                if (!item.torrent_hash.empty() && isLocalBackend()) {
                    releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
                }
                return false;
            }
            if (item.start_future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return true; // Installer still preparing
            }

            auto fut = std::move(item.start_future);
            item.start_future.reset();
            bool started = false;
            std::string exc_msg;
            try {
                started = fut->get();
            } catch (const std::exception& e) {
                exc_msg = e.what();
                util::logLine(std::string("download: exception in hybrid start_future: ") + exc_msg);
                started = false;
            }

            if (!started) {
                item.error_message = (item.hybrid_installer && !item.hybrid_installer->errorMessage().empty())
                                         ? item.hybrid_installer->errorMessage()
                                         : (!exc_msg.empty() ? exc_msg : "failed to start hybrid install");
                util::logLine("download: failed to start hybrid install: " + item.error_message);
                item.hybrid_installer.reset();
                item.state = DownloadState::Failed;
                if (!item.torrent_hash.empty() && isLocalBackend()) {
                    releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
                }
                return false;
            }

            item.auto_hybrid_started = true;
            item.state = DownloadState::StreamInstalling;
            util::logLine("download: hybrid install started for " + item.title +
                          " (index=" + std::to_string(install_file_index) + ")");
            return true;
        }

        if (!item.open_future || !item.open_future->valid()) {
            item.open_future.reset();
            has_open_pending_.store(false);
            item.state = DownloadState::Failed;
            item.error_message = "Stream open task lost.";
            if (!item.torrent_hash.empty() && isLocalBackend()) {
                releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
            }
            return false;
        }

        if (item.open_future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return true; // Still preparing
        }

        auto open_fut = std::move(item.open_future);
        item.open_future.reset();
        has_open_pending_.store(false);
        bool success = false;
        try {
            success = open_fut->get();
        } catch (const std::exception& e) {
            util::logLine(std::string("download: exception in hybrid open_future: ") + e.what());
            success = false;
        }

        if (!success) {
            util::logLine("download: failed to open hybrid stream hash=" + item.torrent_hash +
                          " index=" + std::to_string(install_file_index));
            item.state = DownloadState::Failed;
            item.error_message = "Failed to open stream (metadata timeout or error).";
            if (!item.torrent_hash.empty() && isLocalBackend()) {
                releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
            }
            return false;
        }

        item.hybrid_installer = std::make_unique<installer::HybridNspInstaller>();

        installer::InstallConfig config;
        config.buffer_size = 64 * 1024 * 1024;
        config.chunk_size = 8 * 1024 * 1024;
        config.verify_sha256 = true;
        config.install_ticket = true;
#ifdef __SWITCH__
        std::string loc = config::ConfigManager::instance().getInstallLocation();
        if (loc == "sd") {
            config.storage = NcmStorageId_SdCard;
        } else if (loc == "nand") {
            config.storage = NcmStorageId_BuiltInUser;
        } else { // "auto"
            s64 sdFree = 0;
            s64 nandFree = 0;
            uint64_t needed = source->totalSize();
            util::getStorageFreeSpace(1, sdFree);
            util::getStorageFreeSpace(0, nandFree);
            if (static_cast<s64>(needed) <= sdFree) {
                config.storage = NcmStorageId_SdCard;
            } else if (static_cast<s64>(needed) <= nandFree) {
                config.storage = NcmStorageId_BuiltInUser;
            } else {
                config.storage = NcmStorageId_SdCard; // Default fallback
            }
        }
#endif

        item.hybrid_installer->setSourceFileNameHint(install_stream_name);
        item.hybrid_installer->setHintTitleId(parseTitleIdFromFileName(install_stream_name));

        // Run start() off the progress thread: the header read blocks until the
        // engine delivers the first pieces, and the progress thread must keep
        // polling engine stats for the UI.
        auto installer_ptr = item.hybrid_installer;
        item.start_future = std::make_shared<std::future<bool>>(
            std::async(std::launch::async, [source, installer_ptr, config]() {
                return installer_ptr->start(source, config);
            }));
        return true;
    }

    return true;
}

bool DownloadManager::cancelDownload(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state == DownloadState::Completed ||
        item.state == DownloadState::Cancelled) return false;

    if (item.cancel_flag) {
        item.cancel_flag->store(true);
    }
    if (item.file_dl_cancel) {
        item.file_dl_cancel->store(true);
    }
    if (item.file_dl_pause) {
        item.file_dl_pause->store(false);
    }
    if (item.port_install_cancel) {
        item.port_install_cancel->store(true);
    }
    if (item.port_install_pause) {
        item.port_install_pause->store(false);
    }

    const bool is_active_item = (item.hybrid_installer != nullptr ||
                                 item.file_dl_dispatched ||
                                 item.port_install_dispatched ||
                                 item.state == DownloadState::Downloading ||
                                 item.state == DownloadState::StreamPreparing ||
                                 item.state == DownloadState::StreamInstalling ||
                                 item.state == DownloadState::Installing);

    if (item.hybrid_installer) {
        item.hybrid_installer->cancel();
        item.hybrid_installer.reset();
    }

    // Close the local engine's stream BEFORE joining the futures ONLY if this item
    // was actively downloading/installing. Cancelling a queued or paused item must
    // NOT close the active data source of another running download!
    if (isLocalBackend() && is_active_item) {
        if (auto* source = ds_manager_.getSource()) {
            source->close();
            util::logLine("download: closed local data source after cancel hash=" +
                          item.torrent_hash);
        }
    }

    if (item.open_future) {
        item.open_future.reset();
    }

    if (item.start_future) {
        item.start_future.reset();
    }

    // The file-copy worker polls file_dl_cancel and is unblocked by the
    // source->close() above; reset its future only now so the join cannot hang.
    if (item.file_dl_worker) {
        item.file_dl_worker.reset();
    }
    retirePortWorker(item);   // never wait for the port worker under queue_mutex_
    item.file_dl_dispatched = false;
    item.file_dl_state.reset();
    item.file_dl_dest.clear();
    item.port_install_dispatched = false;
    item.port_install_state.reset();

    has_open_pending_.store(false);

    if (item.stream_consumer_started && item.torrent_id >= 0) {
        stopStreamConsumer(item.torrent_id);
        item.stream_consumer_started = false;
    }
    item.installer.closeStream();

    if (item.torrent_id >= 0 && torrent_) {
        torrent_->cancelTorrent(item.torrent_id);
    } else if (!item.torrent_hash.empty()) {
        releaseTorrentIfUnneeded(queue_, index, item.torrent_hash);
    }
    item.state = DownloadState::Cancelled;
    item.download_speed_kbps = 0.0f;
    item.speed_sample_at = std::chrono::steady_clock::time_point{};
    if (item.finish_time.time_since_epoch().count() == 0) {
        item.finish_time = std::chrono::steady_clock::now();
        if (item.start_time.time_since_epoch().count() > 0) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                item.finish_time - item.start_time).count();
            if (sec > 0) item.elapsed_seconds += sec;
        }
    }
    return true;
}

bool DownloadManager::retryDownload(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state != DownloadState::Failed && item.state != DownloadState::Cancelled) {
        return false;
    }

    if (item.cancel_flag) {
        item.cancel_flag->store(false);
    }
    if (item.file_dl_cancel) {
        item.file_dl_cancel->store(false);
    }
    if (item.file_dl_pause) {
        item.file_dl_pause->store(false);
    }
    // Fresh flags: a cancelled worker that is still finishing keeps the old ones.
    item.port_install_cancel = std::make_shared<std::atomic<bool>>(false);
    item.port_install_pause = std::make_shared<std::atomic<bool>>(false);

    if (item.hybrid_installer) {
        item.hybrid_installer->cancel();
        item.hybrid_installer.reset();
    }
    if (item.open_future) {
        item.open_future.reset();
    }
    if (item.start_future) {
        item.start_future.reset();
    }
    if (item.file_dl_worker) {
        item.file_dl_worker.reset();
    }
    retirePortWorker(item);   // never wait for the port worker under queue_mutex_
    item.file_dl_dispatched = false;
    item.file_dl_state.reset();
    item.file_dl_dest.clear();
    item.port_install_dispatched = false;
    item.port_install_state.reset();
    item.open_done.reset();
    item.open_success.reset();

    item.error_message.clear();
    item.progress = 0.0f;
    item.install_progress = 0.0f;
    item.download_speed_kbps = 0.0f;
    item.install_written = 0;
    item.install_total = 0;
    item.start_time = std::chrono::steady_clock::now();
    item.finish_time = std::chrono::steady_clock::time_point{};
    item.elapsed_seconds = 0;
    item.auto_hybrid_started = false;
    item.stream_consumer_started = false;
    item.preload_started = false;
    item.state = DownloadState::Queued;

    util::logLine("download: retry triggered for index=" + std::to_string(index) + " (" + item.title + ")");

    if (!hasActiveTransfers()) {
        startDownload(index);
    }
    return true;
}

void DownloadManager::retirePortWorker(DownloadItem& item) {
    if (!item.port_install_worker) return;
    if (item.port_install_worker->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { item.port_install_worker->get(); } catch (...) {}
    } else {
        retired_port_workers_.push_back({item.torrent_hash, item.port_install_worker});
        util::logLine("download: port worker of " + item.torrent_hash + " finishes in the background");
    }
    item.port_install_worker.reset();
}

void DownloadManager::reapRetiredWorkers() {
    retired_port_workers_.erase(
        std::remove_if(retired_port_workers_.begin(), retired_port_workers_.end(), [](RetiredWorker& w) {
            if (!w.future || w.future->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try { if (w.future) w.future->get(); } catch (...) {}
                return true;
            }
            return false;
        }),
        retired_port_workers_.end());
}

bool DownloadManager::hasRetiredWorker(const std::string& hash) {
    for (const auto& w : retired_port_workers_) {
        if (equalsIgnoreCase(w.hash, hash)) return true;
    }
    return false;
}

bool DownloadManager::deleteFromQueue(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    const bool failed_port = queue_[index].is_port_auto_install &&
                             queue_[index].state == DownloadState::Failed;
    auto resume = queue_[index].port_resume;
    cancelDownload(index);
    queue_.erase(queue_.begin() + index);
    if (failed_port && resume) {
        // A failed install left files for "Retry"; removing the item removes them
        // (in the background: this runs on the UI thread).
        util::runInBackground([resume]() {
            std::lock_guard<std::mutex> lk(resume->mtx);
            if (resume->journal.fileCount() > 0 || resume->journal.dirCount() > 0) {
                util::logLine("download: removing " + std::to_string(resume->journal.fileCount()) +
                              " files of a failed port install");
                resume->journal.rollback();
            }
        }, "port-rollback");
    }
    return true;
}

bool DownloadManager::pauseDownload(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state == DownloadState::Downloading ||
        item.state == DownloadState::StreamPreparing ||
        item.state == DownloadState::StreamInstalling) {

        if (item.file_dl_pause) {
            item.file_dl_pause->store(true);
        }
        if (item.port_install_pause) {
            item.port_install_pause->store(true);
        }
        if (item.hybrid_installer) {
            item.hybrid_installer->setPaused(true);
        }

        // Note: For streaming installations we do not set cancel_flag=true, as that
        // completely cancels the installer and destroys NCM placeholders.
        // Instead, pausing the torrent engine stops incoming data, causing the installer
        // to safely wait on the buffer until resumed.
        if (!item.torrent_hash.empty()) {
            tsnx_engine_pause_torrent(nullptr, item.torrent_hash.c_str());
        }

        if (item.start_time.time_since_epoch().count() > 0) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - item.start_time).count();
            if (sec > 0) item.elapsed_seconds += sec;
        }
        item.start_time = std::chrono::steady_clock::time_point{};

        item.state = DownloadState::Paused;
        item.download_speed_kbps = 0.0f;
        util::logLine("download: paused index=" + std::to_string(index) + " title=" + item.title);
        return true;
    }
    return false;
}

bool DownloadManager::resumeDownload(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state == DownloadState::Paused || item.state == DownloadState::Installing) {
        if (item.cancel_flag) {
            item.cancel_flag->store(false);
        }
        if (item.file_dl_cancel) {
            item.file_dl_cancel->store(false);
        }
        if (item.file_dl_pause) {
            item.file_dl_pause->store(false);
        }
        if (item.port_install_cancel) {
            item.port_install_cancel->store(false);
        }
        if (item.port_install_pause) {
            item.port_install_pause->store(false);
        }
        if (item.hybrid_installer) {
            item.hybrid_installer->setPaused(false);
        }
        if (!item.torrent_hash.empty()) {
            tsnx_engine_resume_torrent(nullptr, item.torrent_hash.c_str());
        }
        item.start_time = std::chrono::steady_clock::now();
        if (item.hybrid_installer && item.auto_hybrid_started) {
            item.state = DownloadState::StreamInstalling;
        } else {
            item.state = DownloadState::Downloading;
        }
        util::logLine("download: resumed index=" + std::to_string(index) + " title=" + item.title);
        return true;
    }
    return false;
}

bool DownloadManager::getTorrentFiles(size_t index, std::vector<torrent::TorrentFileInfo>& out_files) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];

    if (isLocalBackend()) {
        std::vector<datasource::CustomEngineFileInfo> local_files;
        std::string probe_err;
        if (!datasource::CustomEngineClient::instance().probeFiles(item.torrent_hash, item.magnet, "", local_files, &probe_err)) {
            util::logLine("download: local getTorrentFiles probe failed: " +
                          (probe_err.empty() ? std::string("unknown error") : probe_err));
            return false;
        }
        out_files.clear();
        out_files.reserve(local_files.size());
        for (const auto& lf : local_files) {
            torrent::TorrentFileInfo tf;
            tf.index = lf.index;
            tf.name = lf.path;
            tf.size = lf.size;
            tf.wanted = true;
            out_files.push_back(std::move(tf));
        }
        return !out_files.empty();
    }

    if (item.torrent_id < 0) return false;
    if (!torrent_) return false;
    return torrent_->getTorrentFiles(item.torrent_id, out_files);
}

bool DownloadManager::probeTorrentFiles(const std::string& magnet,
                                        std::vector<torrent::TorrentFileInfo>& out_files,
                                        std::string* out_error) {
    out_files.clear();
    if (out_error) out_error->clear();

    if (magnet.empty()) {
        if (out_error) *out_error = "Empty magnet link";
        return false;
    }

    const std::string link = normalizeTorrentLink(magnet);
    if (isLocalBackend()) {
        std::vector<datasource::CustomEngineFileInfo> local_files;
        std::string probe_err;
        if (!datasource::CustomEngineClient::instance().probeFiles("", link, "", local_files, &probe_err)) {
            util::logLine("download: local probe failed: " +
                          (probe_err.empty() ? std::string("unknown error") : probe_err));

            if (out_error) {
                *out_error = probe_err.empty()
                    ? "Failed to load torrent file list from custom engine"
                    : probe_err;
            }
            return false;
        }

        out_files.reserve(local_files.size());
        for (const auto& lf : local_files) {
            torrent::TorrentFileInfo tf;
            tf.index = lf.index;
            tf.name = lf.path;
            tf.size = lf.size;
            tf.wanted = true;
            out_files.push_back(std::move(tf));
        }

        return !out_files.empty();
    }

    int id = getTorrent()->addMagnet(link);
    if (id < 0) {
        if (out_error) *out_error = "Failed to add torrent";
        return false;
    }

    if (!torrent_ || !torrent_->isServerReachable()) {
        if (torrent_) torrent_->cancelTorrent(id);
        if (out_error) *out_error = "TorrServer is unavailable (file list cannot be fetched)";
        return false;
    }

    constexpr int kMaxAttempts = 72; // ~18s with 250ms polling.
    for (int i = 0; i < kMaxAttempts; ++i) {
        if (torrent_ && torrent_->getTorrentFiles(id, out_files) && !out_files.empty()) {
            if (torrent_) torrent_->cancelTorrent(id);
            return true;
        }
#ifdef __SWITCH__
        svcSleepThread(250000000LL);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
#endif
    }

    if (torrent_) torrent_->cancelTorrent(id);
    if (out_error) *out_error = "Timeout while loading torrent file list";
    return false;
}

bool DownloadManager::setFileWanted(size_t index, int file_index, bool wanted) {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];

    if (isLocalBackend()) {
        return customEngineSetFileWanted(item.torrent_hash, file_index, wanted);
    }

    if (item.torrent_id < 0) return false;
    if (!torrent_) return false;
    return torrent_->setFileWanted(item.torrent_id, file_index, wanted);
}

} // namespace download
