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

    // MSE sess_close() state handling: peer_nb_free() zeros s->nb before the
    // MSE checks, so we must use the saved was_mse / was_mse_active / was_mse_failed
    // flags to avoid infinite MSE retries or clearing MSE-capable peers.
    {
        torrentfs *m = calloc(1, sizeof(*m));
        mutexInit(&m->lock);
        m->freq = armGetSystemTickFreq();
        m->meta.piece_count = 16;
        m->meta.piece_len = 1 << 20;
        m->file_first_piece = 0;
        m->file_last_piece = 15;
        peer_addr addrs[5];
        for (int i = 0; i < 5; i++) {
            addrs[i].ip = 0x0B000001u + (uint32_t)i;
            addrs[i].port = 6881;
        }
        add_peers_src(m, addrs, 5, PEER_SRC_PEX);

        // 0: PEXF_ENCRYPT peer actively rejected MSE -> peer_mse_off=1, immediate plaintext retry
        m->peer_mse_try[0] = 0;
        m->peer_fails[0] = 2;
        sess *s0 = &m->S[0];
        memset(s0, 0, sizeof(*s0));
        s0->active = true;
        s0->connecting = false;
        s0->pidx = 0;
        s0->addr = addrs[0];
        s0->started = armGetSystemTick();
        s0->nb.sock = -1;
        s0->nb.mse_want = true;
        s0->nb.mse_failed = true;
        s0->nb.mse_active = false;
        s0->nb.handshaked = false;
        sess_close(m, s0, true);
        bool c0 = (m->peer_mse_off[0] == 1 && m->peer_fails[0] == 0);

        // 1: peer already failed plaintext (peer_mse_try=1) and silently timed out on MSE
        // -> peer_mse_off=1 AND backoff preserved (peer_fails > 0, peer_next_try > now)
        m->peer_mse_try[1] = 1;
        m->peer_fails[1] = 1;
        sess *s1 = &m->S[1];
        memset(s1, 0, sizeof(*s1));
        s1->active = true;
        s1->connecting = false;
        s1->pidx = 1;
        s1->addr = addrs[1];
        s1->started = armGetSystemTick();
        s1->nb.sock = -1;
        s1->nb.mse_want = true;
        s1->nb.mse_failed = false;
        s1->nb.mse_active = false;
        s1->nb.handshaked = false;
        s1->fail_kind = PEER_FAIL_TIMEOUT;
        u64 before1 = armGetSystemTick();
        sess_close(m, s1, true);
        bool c1 = (m->peer_mse_off[1] == 1 && m->peer_fails[1] > 1 && m->peer_next_try[1] > before1);

        // 2: peer succeeded with MSE -> must NOT get peer_mse_off=1
        sess *s2 = &m->S[2];
        memset(s2, 0, sizeof(*s2));
        s2->active = true;
        s2->connecting = false;
        s2->pidx = 2;
        s2->addr = addrs[2];
        s2->started = armGetSystemTick();
        s2->nb.sock = -1;
        s2->nb.mse_want = true;
        s2->nb.mse_active = true;
        s2->nb.handshaked = true;
        m->st_live = 1;
        sess_close(m, s2, false);
        bool c2 = (m->peer_mse_off[2] == 0);

        // 3: peer succeeded with plaintext -> peer_mse_off=1
        sess *s3 = &m->S[3];
        memset(s3, 0, sizeof(*s3));
        s3->active = true;
        s3->connecting = false;
        s3->pidx = 3;
        s3->addr = addrs[3];
        s3->started = armGetSystemTick();
        s3->nb.sock = -1;
        s3->nb.mse_want = false;
        s3->nb.handshaked = true;
        m->st_live = 1;
        sess_close(m, s3, false);
        bool c3 = (m->peer_mse_off[3] == 1);

        // 4: connect timeout before TCP/uTP came up -> peer_mse_off must stay 0
        sess *s4 = &m->S[4];
        memset(s4, 0, sizeof(*s4));
        s4->active = true;
        s4->connecting = true;
        s4->pidx = 4;
        s4->addr = addrs[4];
        s4->started = armGetSystemTick();
        s4->nb.sock = -1;
        s4->nb.mse_want = true;
        s4->nb.handshaked = false;
        m->st_connecting = 1;
        sess_close(m, s4, true);
        bool c4 = (m->peer_mse_off[4] == 0);

        bool mse_ok = c0 && c1 && c2 && c3 && c4;
        printf("mse sess_close: c0=%d c1=%d c2=%d c3=%d c4=%d %s\n",
               c0, c1, c2, c3, c4, mse_ok ? "PASSED" : "FAILED");
        free(m);
        if (!mse_ok) fails++;
    }

    return fails ? 1 : 0;
}
