#ifndef TSNX_APP_PATHS_H
#define TSNX_APP_PATHS_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define TSNX_MKDIR(p) _mkdir(p)
#else
#define TSNX_MKDIR(p) mkdir((p), 0777)
#endif

/*
 * Central storage layout for TorrentShopNX.
 *
 * Base directory on Switch: sdmc:/switch/TorrentShopNX  (PC: ./)
 *   - config.ini, log.txt, engine.log, app .nro stay at the root
 *   - cache/  - regenerable data, split into per-type subfolders
 *   - data/   - persistent user state / downloaded databases
 *   - downloads/ - files downloaded by the user (kept at root, user-facing)
 */

#ifdef __SWITCH__
#define TSNX_BASE_DIR "sdmc:/switch/TorrentShopNX"
#else
#define TSNX_BASE_DIR "."
#endif

#define TSNX_CACHE_DIR           TSNX_BASE_DIR "/cache"
#define TSNX_CACHE_CATALOG       TSNX_CACHE_DIR "/catalog"
#define TSNX_CACHE_THUMBNAILS    TSNX_CACHE_DIR "/thumbnails"
#define TSNX_CACHE_COLLECTIONS   TSNX_CACHE_DIR "/collections"
#define TSNX_CACHE_META          TSNX_CACHE_DIR "/meta"
#define TSNX_CACHE_LOCALENGINE   TSNX_CACHE_DIR "/local_engine"
#define TSNX_CACHE_LOCALENGINE_OLD TSNX_CACHE_DIR "/local_engine_old"
#define TSNX_CACHE_TORRENTFS     TSNX_CACHE_DIR "/torrentfs"
#define TSNX_CACHE_DHT           TSNX_CACHE_DIR "/dht"
#define TSNX_CACHE_ICONS         TSNX_CACHE_DIR "/icons"
#define TSNX_CACHE_STREAM        TSNX_CACHE_DIR "/stream_install"
#define TSNX_CACHE_TMP           TSNX_CACHE_DIR "/tmp"

#define TSNX_DATA_DIR            TSNX_BASE_DIR "/data"
#define TSNX_DOWNLOADS_DIR       TSNX_BASE_DIR "/downloads"

#define TSNX_CONFIG_PATH         TSNX_BASE_DIR "/config.ini"
#define TSNX_LOG_PATH            TSNX_BASE_DIR "/log.txt"
#define TSNX_ENGINE_LOG_PATH     TSNX_BASE_DIR "/engine.log"

#define TSNX_CATALOG_JSON_RU     TSNX_DATA_DIR "/switch_games_ru.json"
#define TSNX_CATALOG_BIN_RU      TSNX_DATA_DIR "/switch_games_ru.bin"
#define TSNX_CATALOG_JSON_EN     TSNX_DATA_DIR "/switch_games_en.json"
#define TSNX_CATALOG_BIN_EN      TSNX_DATA_DIR "/switch_games_en.bin"

#define TSNX_VERSIONS_PATH       TSNX_DATA_DIR "/versions.txt"
#define TSNX_FAVORITES_PATH      TSNX_DATA_DIR "/favorites.json"
#define TSNX_SOURCES_PATH        TSNX_DATA_DIR "/sources.json"

#define TSNX_DHT_CACHE_FILE      TSNX_CACHE_DHT "/dht_cache.bin"
#define TSNX_TORRENTFS_CACHE     TSNX_CACHE_TORRENTFS "/cache.bin"
#define TSNX_TEMP_UPLOAD         TSNX_CACHE_TMP "/upload.torrent"

/* Legacy (pre-reorg) locations - used by the one-time startup migration. */
#define TSNX_OLD_CATALOG_JSON_RU TSNX_BASE_DIR "/switch_games_ru.json"
#define TSNX_OLD_CATALOG_BIN_RU  TSNX_BASE_DIR "/switch_games_ru.bin"
#define TSNX_OLD_CATALOG_JSON_EN TSNX_BASE_DIR "/switch_games_en.json"
#define TSNX_OLD_CATALOG_BIN_EN  TSNX_BASE_DIR "/switch_games_en.bin"
#define TSNX_OLD_CATALOG_JSON    TSNX_BASE_DIR "/switch_games.json"
#define TSNX_OLD_CATALOG_BIN     TSNX_BASE_DIR "/switch_games.bin"
#define TSNX_OLD_VERSIONS        TSNX_BASE_DIR "/versions.txt"
#define TSNX_OLD_FAVORITES       TSNX_BASE_DIR "/favorites.json"
#define TSNX_OLD_SOURCES         TSNX_BASE_DIR "/sources.json"
#define TSNX_OLD_DHT_CACHE       TSNX_BASE_DIR "/dht_cache.bin"
#define TSNX_OLD_TORRENTFS_CACHE TSNX_BASE_DIR "/cache.bin"
#define TSNX_OLD_COLLECTIONS     TSNX_BASE_DIR "/collections"
#define TSNX_OLD_META            TSNX_BASE_DIR "/meta"
#define TSNX_OLD_ICONS           TSNX_BASE_DIR "/icons"
#define TSNX_OLD_STREAM_INSTALL  TSNX_BASE_DIR "/stream_install"
#define TSNX_OLD_TEMP_UPLOAD     TSNX_BASE_DIR "/temp_upload.torrent"

/*
 * Best-effort creation of the parent directory chain for `path` (several
 * engines write into cache/<type>/ subfolders that may not exist yet).
 * Failures are ignored: the actual open()/fopen() reports the real error.
 * C and C++ safe.
 */
static inline void tsnx_ensure_parent_dirs(const char *path) {
    char dir[512];
    size_t len = strlen(path);
    if (len >= sizeof(dir)) return;
    memcpy(dir, path, len + 1);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    char cur[512];
    size_t pos = 0;
    cur[0] = '\0';
    if (strncmp(dir, "sdmc:/", 6) == 0) {
        snprintf(cur, sizeof(cur), "sdmc:");
        pos = 6;
    }
    while (pos <= strlen(dir)) {
        size_t next = pos;
        while (dir[next] && dir[next] != '/') next++;
        if (next > pos) {
            size_t clen = strlen(cur);
            if (clen && cur[clen - 1] != '/') cur[clen++] = '/';
            if (clen + (next - pos) + 1 <= sizeof(cur)) {
                memcpy(cur + clen, dir + pos, next - pos);
                cur[clen + (next - pos)] = '\0';
                TSNX_MKDIR(cur);
            }
        }
        if (!dir[next]) break;
        pos = next + 1;
    }
}

#endif /* TSNX_APP_PATHS_H */
