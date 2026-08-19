#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "engine.h"

static volatile bool g_stop = false;

static void on_signal(int sig) {
    (void)sig;
    g_stop = true;
}

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static int choose_largest_file(tsnx_engine *eng, const char *hash) {
    tsnx_file_info files[TSNX_MAX_FILES];
    int n = tsnx_engine_get_files(eng, hash, files, TSNX_MAX_FILES);
    if (n <= 0) return -1;
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (files[i].size > files[best].size) best = i;
    }
    return best;
}

int main(int argc, char **argv) {
    const char *magnet =
        "magnet:?xt=urn:btih:838E7B98569A3C00C8B868B6E362468F5F78AA7B"
        "&tr=http%3A%2F%2Fbt2.t-ru.org%2Fann%3Fmagnet"
        "&dn=%5BNintendo%20Switch%5D%20The%20Legend%20of%20Zelda%3A%20Skyward%20Sword%20HD%20%5BNSZ%5D%5BRUS%2FMulti9%5D";
    int file_index = -1;
    int duration_sec = 120;

    if (argc > 1) {
        if (strncmp(argv[1], "magnet:", 7) == 0) {
            magnet = argv[1];
            if (argc > 2) file_index = atoi(argv[2]);
            if (argc > 3) duration_sec = atoi(argv[3]);
        } else {
            duration_sec = atoi(argv[1]);
            if (argc > 2 && strncmp(argv[2], "magnet:", 7) == 0) {
                magnet = argv[2];
                if (argc > 3) file_index = atoi(argv[3]);
            } else if (argc > 2) {
                file_index = atoi(argv[2]);
                if (argc > 3) magnet = argv[3];
            }
        }
    }

    signal(SIGINT, on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    printf("StreamTest: starting engine\n");
    fflush(stdout);
    tsnx_engine *eng = tsnx_engine_start(6881);
    if (!eng) {
        fprintf(stderr, "failed to start engine\n");
        return 1;
    }

    char hash[41] = {0};
    printf("StreamTest: adding magnet...\n");
    fflush(stdout);
    if (!tsnx_engine_add_magnet(eng, magnet, hash, sizeof(hash))) {
        fprintf(stderr, "failed to add magnet\n");
        tsnx_engine_stop(eng);
        return 1;
    }
    printf("StreamTest: hash=%s\n", hash);
    fflush(stdout);

    if (file_index < 0) {
        file_index = choose_largest_file(eng, hash);
        printf("StreamTest: selected largest file index=%d\n", file_index);
    }

    tsnx_file_info files[TSNX_MAX_FILES];
    int nfiles = tsnx_engine_get_files(eng, hash, files, TSNX_MAX_FILES);
    int64_t file_size = 0;
    if (file_index >= 0 && file_index < nfiles) {
        file_size = files[file_index].size;
        printf("StreamTest: file[%d] size=%lld path=%s\n",
               file_index, (long long)file_size, files[file_index].path);
    }

    printf("StreamTest: prepare stream file=%d\n", file_index);
    fflush(stdout);
    if (!tsnx_engine_prepare_stream(eng, hash, file_index)) {
        fprintf(stderr, "failed to prepare stream\n");
        tsnx_engine_remove_torrent(eng, hash);
        tsnx_engine_stop(eng);
        return 1;
    }

    int piece_size = tsnx_engine_piece_size(eng, hash);
    printf("StreamTest: piece_size=%d file_size=%lld\n",
           piece_size, (long long)file_size);
    fflush(stdout);

    uint8_t *buf = malloc(256 * 1024);
    if (!buf) {
        fprintf(stderr, "out of memory\n");
        tsnx_engine_remove_torrent(eng, hash);
        tsnx_engine_stop(eng);
        return 1;
    }

    uint64_t start = now_ms();
    uint64_t last_stats = start;
    int64_t total_read = 0;
    int64_t offset = 0;
    const int64_t chunk = 256 * 1024;
    int no_data_sec = 0;

    while (!g_stop) {
        int64_t to_read = chunk;
        if (file_size > 0 && offset + to_read > file_size)
            to_read = file_size - offset;
        if (to_read <= 0) {
            offset = 0;
            to_read = chunk;
            if (file_size > 0 && to_read > file_size) to_read = file_size;
        }

        int64_t got = tsnx_engine_read(eng, hash, offset, buf, to_read);
        if (got < 0) {
            printf("StreamTest: read error at offset=%lld\n", (long long)offset);
            break;
        }
        if (got > 0) {
            offset += got;
            total_read += got;
            tsnx_engine_set_min_keep_offset(eng, hash, offset);
            no_data_sec = 0;
        } else {
            sleep_ms(50);
        }

        uint64_t now = now_ms();
        if (now - last_stats >= 1000) {
            tsnx_torrent_item items[8];
            int n = tsnx_engine_get_torrents(eng, items, 8);
            tsnx_torrent_item *it = NULL;
            for (int i = 0; i < n; i++) {
                if (strcmp(items[i].hash, hash) == 0) { it = &items[i]; break; }
            }
            double elapsed = (now - start) / 1000.0;
            double mb = total_read / (1024.0 * 1024.0);
            printf("[%.1fs] peers=%d speed=%.1fKB/s progress=%.2f%% read=%.2fMB offset=%lld",
                   elapsed,
                   it ? it->peers : 0,
                   it ? it->download_kbps : 0.0,
                   it ? (it->progress * 100.0) : 0.0,
                   mb,
                   (long long)offset);
            if (!it || it->download_kbps < 1.0f) {
                no_data_sec++;
                printf(" nodata=%d", no_data_sec);
            } else {
                no_data_sec = 0;
            }
            printf("\n");
            fflush(stdout);
            last_stats = now;

            if ((int)elapsed >= duration_sec) {
                printf("StreamTest: duration reached\n");
                break;
            }
            if (no_data_sec >= 30) {
                printf("StreamTest: no data for 30s, giving up\n");
                break;
            }
        }
    }

    printf("StreamTest: shutting down\n");
    tsnx_engine_cancel_read(eng, hash);
    free(buf);
    tsnx_engine_remove_torrent(eng, hash);
    tsnx_engine_stop(eng);
    printf("StreamTest: done\n");
    return 0;
}
