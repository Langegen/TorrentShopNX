// Peer pool index fuzz test: drives add_peers_src() (tracker / PEX / DHT
// batches, duplicates, a full pool with evictions, busy entries) and checks
// after every batch that the IP -> slot hash index matches the pool exactly.
// Includes torrentfs.c to reach its static functions; links against the other
// engine objects.
//
//   pctest\test_pool.exe

#include "../source/engine/torrentfs.c"

static unsigned rng_state = 12345u;
static unsigned rnd(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 1;
}

static int check(torrentfs *t, const char *when) {
    int used = 0;
    for (int h = 0; h < POOL_HASH_SIZE; h++) if (t->pool_hash[h]) used++;
    if (used != t->peer_count) {
        printf("FAIL (%s): index has %d entries, pool %d\n", when, used, t->peer_count);
        return 1;
    }
    for (int i = 0; i < t->peer_count; i++) {
        int f = pool_find(t, t->peers[i].ip, t->peers[i].port);
        if (f != i) {
            printf("FAIL (%s): slot %d %08x:%u found at %d\n", when, i, t->peers[i].ip,
                   t->peers[i].port, f);
            return 1;
        }
        for (int j = i + 1; j < t->peer_count; j++)
            if (t->peers[j].ip == t->peers[i].ip && t->peers[j].port == t->peers[i].port) {
                printf("FAIL (%s): duplicate address in slots %d and %d\n", when, i, j);
                return 1;
            }
    }
    return 0;
}

int main(void) {
    torrentfs *t = calloc(1, sizeof(*t));
    mutexInit(&t->lock);
    t->freq = armGetSystemTickFreq();
    int fails = 0;
    peer_addr batch[512];

    for (int round = 0; round < 400 && !fails; round++) {
        // Small IP universe => lots of duplicates; it grows so the pool fills
        // up and evictions start.
        unsigned universe = 600 + (unsigned)round * 20;
        int n = 1 + (int)(rnd() % 512);
        for (int k = 0; k < n; k++) {
            batch[k].ip = 0x0A000000u + rnd() % universe;
            batch[k].port = (uint16_t)(6881 + rnd() % 4);   // few ports: same-IP clients
        }
        int src = (int)(rnd() % 3);
        add_peers_src(t, batch, n, src);

        // Random failures / busy marks so eviction scores vary.
        for (int k = 0; k < 64 && t->peer_count > 0; k++) {
            int i = (int)(rnd() % (unsigned)t->peer_count);
            t->peer_fails[i] = (uint8_t)(rnd() % 10);
            t->peer_busy[i] = (uint8_t)(rnd() % 8 == 0);
            t->peer_ok[i] = (uint8_t)(rnd() % 16 == 0);
        }
        char when[32];
        snprintf(when, sizeof(when), "round %d", round);
        fails += check(t, when);
    }
    printf("pool fuzz: %d entries in the end, %s\n", t->peer_count, fails ? "FAILED" : "PASSED");
    free(t);

    // Lookahead vs file selection: two files of 40 pieces each; streaming
    // file 0 near its end, the first pieces of file 1 are pre-fetched only
    // while file 1 is wanted.
    torrentfs *u = calloc(1, sizeof(*u));
    mutexInit(&u->lock);
    torrent_file files[2] = {{40LL << 20, 0, "a"}, {40LL << 20, 40LL << 20, "b"}};
    u->meta.files = files;
    u->meta.file_count = 2;
    u->meta.piece_len = 1 << 20;
    u->meta.piece_count = 80;
    u->file_first_piece = 0;
    u->file_last_piece = 39;
    u->playhead_piece = 37;          // within 4 pieces of the end
    u->read_blocked_piece = -1;
    u->piece_zone = calloc(80, 1);
    u->file_unwanted = calloc(2, 1);
    u->n_aq = 48;
    int wanted_prio = piece_priority(u, 41);
    torrentfs_set_file_wanted(u, 1, false);
    int unwanted_prio = piece_priority(u, 41);
    torrentfs_set_file_wanted(u, 1, true);
    int again_prio = piece_priority(u, 41);
    bool la_ok = wanted_prio == 5 && unwanted_prio == 0 && again_prio == 5 &&
                 piece_priority(u, 39) > 0;   // the streamed file itself is untouched
    printf("lookahead vs selection: wanted=%d unwanted=%d re-wanted=%d %s\n",
           wanted_prio, unwanted_prio, again_prio, la_ok ? "PASSED" : "FAILED");
    free(u->piece_zone);
    free(u->file_unwanted);
    free(u);
    if (!la_ok) fails++;
    return fails ? 1 : 0;
}
