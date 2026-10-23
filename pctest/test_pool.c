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
        int f = pool_find(t, t->peers[i].ip);
        if (f != i) {
            printf("FAIL (%s): slot %d ip %08x found at %d\n", when, i, t->peers[i].ip, f);
            return 1;
        }
        for (int j = i + 1; j < t->peer_count; j++)
            if (t->peers[j].ip == t->peers[i].ip) {
                printf("FAIL (%s): duplicate ip in slots %d and %d\n", when, i, j);
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
            batch[k].port = (uint16_t)(1000 + rnd() % 60000);
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
    return fails ? 1 : 0;
}
