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
#include "torrent_meta.h"
#include "dhtclient.h"

static volatile bool g_stop = false;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

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

static void engine_log(const char *msg) {
    fprintf(stderr, "[engine] %s\n", msg);
    fflush(stderr);
}

// Apply a simple 5-zone schedule around the current read offset.
static void apply_zones(tsnx_engine *eng, const char *hash,
                        int64_t offset, int piece_size,
                        int64_t file_first_piece, int64_t file_last_piece) {
    if (piece_size <= 0) return;
    int current = (int)(offset / piece_size);
    if (current < file_first_piece) current = file_first_piece;
    if (current > file_last_piece) current = file_last_piece;

    tsnx_engine_clear_piece_zones(eng, hash);

    int c0 = clamp_int(current, file_first_piece, file_last_piece);
    int c1 = clamp_int(current + 1, file_first_piece, file_last_piece);
    tsnx_engine_set_piece_zone(eng, hash, c0, c1 - c0 + 1, TSNX_ZONE_CRITICAL);

    int u0 = clamp_int(c1 + 1, file_first_piece, file_last_piece);
    int u1 = clamp_int(u0 + 2, file_first_piece, file_last_piece);
    if (u1 >= u0)
        tsnx_engine_set_piece_zone(eng, hash, u0, u1 - u0 + 1, TSNX_ZONE_URGENT);

    int p0 = clamp_int(u1 + 1, file_first_piece, file_last_piece);
    int p1 = clamp_int(p0 + 7, file_first_piece, file_last_piece);
    if (p1 >= p0)
        tsnx_engine_set_piece_zone(eng, hash, p0, p1 - p0 + 1, TSNX_ZONE_PREFETCH);

    int s0 = clamp_int(p1 + 1, file_first_piece, file_last_piece);
    int s1 = clamp_int(s0 + 15, file_first_piece, file_last_piece);
    if (s1 >= s0)
        tsnx_engine_set_piece_zone(eng, hash, s0, s1 - s0 + 1, TSNX_ZONE_SPECULATIVE);

    int t1 = clamp_int(c0 - 1, file_first_piece, file_last_piece);
    int t0 = clamp_int(c0 - 4, file_first_piece, file_last_piece);
    if (t1 >= t0)
        tsnx_engine_set_piece_zone(eng, hash, t0, t1 - t0 + 1, TSNX_ZONE_TAIL);
}

int main(int argc, char **argv) {
    const char *magnet =
        "magnet:?xt=urn:btih:BF716396E30C82C8BFF8DFD1F3A51569649D31FF"
        "&tr=http%3A%2F%2Fbt2.t-ru.org%2Fann%3Fmagnet"
        "&dn=%5BNintendo%20Switch%5D%20Letter%20Quest%20Remastered%20%5BNSP%5D%5BENG%5D";
    int file_index = -1;
    int duration_sec = 300;

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
            }
        }
    }

    signal(SIGINT, on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    torrent_set_log(engine_log);
    dht_set_log(engine_log);

    printf("DiagTest: starting engine\n");
    tsnx_engine *eng = tsnx_engine_start(6881);
    if (!eng) {
        fprintf(stderr, "failed to start engine\n");
        return 1;
    }
    tsnx_engine_set_governor(eng, 0);
    tsnx_engine_set_ram_stream(eng, 1);

    char hash[41] = {0};
    printf("DiagTest: adding magnet...\n");
    if (!tsnx_engine_add_magnet(eng, magnet, hash, sizeof(hash))) {
        fprintf(stderr, "failed to add magnet\n");
        tsnx_engine_stop(eng);
        return 1;
    }
    printf("DiagTest: hash=%s\n", hash);

    if (file_index < 0) {
        file_index = choose_largest_file(eng, hash);
        printf("DiagTest: selected largest file index=%d\n", file_index);
    }

    tsnx_file_info files[TSNX_MAX_FILES];
    int nfiles = tsnx_engine_get_files(eng, hash, files, TSNX_MAX_FILES);
    int64_t file_size = 0;
    int64_t file_offset = 0;
    if (file_index >= 0 && file_index < nfiles) {
        file_size = files[file_index].size;
        file_offset = files[file_index].offset;
        printf("DiagTest: file[%d] size=%lld offset=%lld path=%s\n",
               file_index, (long long)file_size, (long long)file_offset,
               files[file_index].path);
    }

    printf("DiagTest: prepare stream file=%d\n", file_index);
    if (!tsnx_engine_prepare_stream(eng, hash, file_index)) {
        fprintf(stderr, "failed to prepare stream\n");
        tsnx_engine_remove_torrent(eng, hash);
        tsnx_engine_stop(eng);
        return 1;
    }

    int piece_size = tsnx_engine_piece_size(eng, hash);
    printf("DiagTest: piece_size=%d file_size=%lld\n",
           piece_size, (long long)file_size);

    int64_t file_first_piece = 0, file_last_piece = 0;
    if (piece_size > 0 && file_size > 0) {
        file_first_piece = file_offset / piece_size;
        file_last_piece = (file_offset + file_size - 1) / piece_size;
    }

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
        apply_zones(eng, hash, offset, piece_size,
                    file_first_piece, file_last_piece);

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
            printf("DiagTest: read error at offset=%lld\n", (long long)offset);
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
            tsnx_engine_diag d;
            bool have_diag = tsnx_engine_get_diag(eng, hash, &d);
            double elapsed = (now - start) / 1000.0;
            double mb = total_read / (1024.0 * 1024.0);
            printf("[%.1fs] peers=%d live=%d peak=%d conn=%d claiming=%d idle=%d "
                   "speed=%.1fKB/s progress=%.2f%% read=%.2fMB offset=%lld "
                   "calm=%d empty_bf=%d sock_fail=%d timeouts=%d "
                   "dht_peers=%d dht_nodes=%d/%d piece=%ld %d/%d/%d\n",
                   elapsed,
                   have_diag ? d.peers : 0,
                   have_diag ? d.live : 0,
                   have_diag ? d.peak : 0,
                   have_diag ? d.connecting : 0,
                   have_diag ? d.claiming : 0,
                   have_diag ? d.idle : 0,
                   have_diag ? d.download_kbps : 0.0f,
                   have_diag && d.pieces_total > 0
                       ? (double)d.pieces_done / d.pieces_total * 100.0
                       : 0.0,
                   mb,
                   (long long)offset,
                   have_diag ? d.calm : 0,
                   have_diag ? d.empty_bitfield : 0,
                   have_diag ? d.sock_fail : 0,
                   have_diag ? d.timeouts : 0,
                   have_diag ? d.dht_peers : 0,
                   have_diag ? d.dht_good : 0,
                   have_diag ? d.dht_dubious : 0,
                   have_diag ? (long)d.playhead_piece : 0L,
                   have_diag ? d.piece_have : 0,
                   have_diag ? d.piece_req : 0,
                   have_diag ? d.piece_total : 0);
            fflush(stdout);
            last_stats = now;

            if ((int)elapsed >= duration_sec) {
                printf("DiagTest: duration reached\n");
                break;
            }
            if (no_data_sec >= 60) {
                printf("DiagTest: no data for 60s, giving up\n");
                break;
            }
        }
    }

    printf("DiagTest: shutting down\n");
    tsnx_engine_cancel_read(eng, hash);
    free(buf);
    tsnx_engine_remove_torrent(eng, hash);
    tsnx_engine_stop(eng);
    printf("DiagTest: done\n");
    return 0;
}
