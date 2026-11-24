#include "meta_fetch.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>
#include <mbedtls/sha1.h>

#include "bencode.h"
#include "bt_peer.h"
#include "dhtclient.h"
#include "engine_log.h"
#include "thread_cfg.h"
#include "utp_nb.h"

#define META_PIECE_LEN        16384
#define META_MAX_SIZE         (4 * 1024 * 1024)
#define META_RX_CAP           (64 * 1024)
#define META_MAX_PEERS        256
#ifdef __SWITCH__
#define META_HARD_MAX_SESS    16
#define META_DEFAULT_SESS     16
#define META_UTP_MAX_DIAL     4
#else
#define META_HARD_MAX_SESS    32
#define META_DEFAULT_SESS     32
#define META_UTP_MAX_DIAL     6
#endif
#define META_SINGLE_BUDGET_MS 12000
#define META_DEFAULT_BUDGET_MS 50000
#define META_EXTRA_PROGRESS_MS 15000
#define META_TCP_CONN_MS      3000
#define META_HS_WAIT_MS       3500
#define META_UTP_CONN_MS      5000
#define META_EXT_HS_WAIT_MS   6000
#define META_PEX_LINGER_MS    4000
#define META_PIECE_REQ_MS     6000
#define META_REQ_WINDOW       4
#define META_INC_QUEUE_MAX    8
#define META_ACTIVE_REG_MAX   4

#define PEXF_ENCRYPT          0x01

enum {
    PIECE_NEEDED   = 0,
    PIECE_INFLIGHT = 1,
    PIECE_DONE     = 2,
};

enum {
    DIAL_PLAIN = 0,
    DIAL_MSE   = 1,
    DIAL_UTP   = 2,
};

typedef struct {
    peer_addr addr;
    uint8_t   prio;        /* incoming=3, tracker=2, pex=1, dht=0 */
    uint8_t   flags;       /* PEXF_ENCRYPT */
    bool      tried_plain;
    bool      tried_mse;
    bool      tried_utp;
    bool      need_mse;
    bool      need_plain;
    bool      need_utp;
    bool      busy;
    bool      bad;
    bool      counted_tried;
    bool      served_meta;
    uint8_t   mse_prio;    /* 0=none, 1=pre-hs timeout, 2=active close/RST */
    uint64_t  next_try_ms;
} meta_pool_entry;

typedef struct {
    bool      active;
    bool      connecting;
    bool      tcp_connected;
    bool      incoming;
    bool      use_mse;
    bool      use_utp;
    bool      ext_sent;
    bool      ext_received;
    bool      contributed;
    bool      meta_rejected;
    bool      pre_hs_timeout;
    int       pidx;
    peer_addr addr;
    peer_nb   nb;
    uint64_t  started_ms;
    uint64_t  connected_ms;
    uint64_t  conn_deadline_ms;
    uint64_t  ext_hs_deadline_ms;
    uint64_t  pex_until_ms;
    uint64_t  last_rx_ms;

    uint8_t   meta_id;
    uint8_t   pex_id;
    int64_t   meta_size;

    int       req_pieces[META_REQ_WINDOW];
    int       req_cnt;
    char      close_why[64];
} meta_sess;

typedef struct {
    int      fd;
    void    *utp;
    uint32_t ip;
    uint16_t port;
    uint8_t  hs[68];
} meta_inc_item;

typedef struct {
    uint8_t              info_hash[20];
    uint8_t              peer_id[20];
    const volatile bool *cancel;

    Mutex                mu;
    meta_pool_entry      pool[META_MAX_PEERS];
    int                  pool_count;
    bool                 done;
    volatile bool        stop_disc;
    int                  max_sess;

    meta_sess            S[META_HARD_MAX_SESS];

    /* Incoming connections routed from listener / utp_nb */
    meta_inc_item        inc[META_INC_QUEUE_MAX];
    int                  inc_n;

    /* Shared piece assembly buffer */
    bool                 asm_init;
    size_t               asm_size;
    int                  npieces;
    int                  done_pieces;
    uint8_t             *asm_buf;
    uint8_t             *piece_state;
    uint64_t            *piece_req_ms;
    uint64_t             last_progress_ms;

    /* Discovery tracking */
    torrent_meta        *announce_stub;
    Thread               disc_thread;
    bool                 disc_started;

    /* Scheduling & diagnostic stats */
    uint32_t             dial_seq;
    int                  stat_dial[3];
    int                  stat_tcp_ok;
    int                  stat_bt_hs;
    int                  stat_ext_ok;
    int                  stat_hs_to;
    int                  stat_no_hs;
} meta_ctx;

/* Registry of active metadata fetches so incoming TCP/uTP connections can route to them */
static Mutex             g_reg_mtx;
static int               g_reg_init = 0;
static meta_ctx         *g_active[META_ACTIVE_REG_MAX];
static meta_utp_route_fn g_utp_router = NULL;
static volatile bool     g_low_concurrency = false;

void meta_fetch_init_early(void) {
    if (!g_reg_init) {
        mutexInit(&g_reg_mtx);
        g_reg_init = 1;
    }
}

void meta_fetch_set_low_concurrency(bool low) {
    g_low_concurrency = low;
}

void meta_fetch_set_utp_router(meta_utp_route_fn fn) {
    g_utp_router = fn;
}

static uint64_t mf_now_ms(void) {
    u64 f = armGetSystemTickFreq();
    u64 t = armGetSystemTick();
    if (!f) return 0;
    return (uint64_t)((t / f) * 1000ULL + ((t % f) * 1000ULL) / f);
}

static void mf_set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

static bool mf_cancelled(const meta_ctx *ctx) {
    return ctx->cancel && *ctx->cancel;
}

static void mf_note_err(meta_ctx *ctx, const char *msg, bool connected) {
    (void)ctx;
    if (connected) torrent_meta_connected++;
    if (msg && msg[0]) {
        snprintf(torrent_meta_last_err, sizeof(torrent_meta_last_err), "%s", msg);
    }
}

static void reg_add(meta_ctx *ctx) {
    meta_fetch_init_early();
    mutexLock(&g_reg_mtx);
    for (int i = 0; i < META_ACTIVE_REG_MAX; i++) {
        if (!g_active[i]) {
            g_active[i] = ctx;
            break;
        }
    }
    mutexUnlock(&g_reg_mtx);
}

static void reg_remove(meta_ctx *ctx) {
    meta_fetch_init_early();
    mutexLock(&g_reg_mtx);
    for (int i = 0; i < META_ACTIVE_REG_MAX; i++) {
        if (g_active[i] == ctx) {
            g_active[i] = NULL;
        }
    }
    mutexUnlock(&g_reg_mtx);
}

static bool inc_push(meta_ctx *ctx, int fd, void *utp, uint32_t ip, uint16_t port,
                     const uint8_t hs68[68]) {
    bool ok = false;
    mutexLock(&ctx->mu);
    if (!ctx->done && !ctx->stop_disc && ctx->inc_n < META_INC_QUEUE_MAX) {
        meta_inc_item *it = &ctx->inc[ctx->inc_n++];
        it->fd   = fd;
        it->utp  = utp;
        it->ip   = ip;
        it->port = port;
        memcpy(it->hs, hs68, 68);
        ok = true;
    }
    mutexUnlock(&ctx->mu);
    return ok;
}

bool meta_fetch_route_incoming(const uint8_t info_hash[20], int fd, const uint8_t hs68[68]) {
    if (!info_hash || !hs68 || fd < 0) return false;
    meta_fetch_init_early();
    bool ok = false;
    mutexLock(&g_reg_mtx);
    for (int i = 0; i < META_ACTIVE_REG_MAX; i++) {
        meta_ctx *c = g_active[i];
        if (c && memcmp(c->info_hash, info_hash, 20) == 0) {
            ok = inc_push(c, fd, NULL, 0, 0, hs68);
            if (ok) break;
        }
    }
    mutexUnlock(&g_reg_mtx);
    return ok;
}

bool meta_fetch_route_incoming_utp(const uint8_t info_hash[20], void *utp,
                                   const uint8_t hs68[68],
                                   uint32_t ip_net, uint16_t port_host) {
    if (!info_hash || !hs68 || !utp) return false;
    meta_fetch_init_early();
    bool ok = false;
    mutexLock(&g_reg_mtx);
    for (int i = 0; i < META_ACTIVE_REG_MAX; i++) {
        meta_ctx *c = g_active[i];
        if (c && memcmp(c->info_hash, info_hash, 20) == 0) {
            ok = inc_push(c, -1, utp, ip_net, port_host, hs68);
            if (ok) break;
        }
    }
    mutexUnlock(&g_reg_mtx);
    return ok;
}

/* Add peers to ctx->pool (thread-safe: called from tracker, DHT, and PEX). */
static void pool_add_locked(meta_ctx *ctx, const peer_addr *peers, int n,
                            uint8_t prio, const uint8_t *flags) {
    if (!peers || n <= 0) return;
    for (int i = 0; i < n; i++) {
        if (peers[i].ip == 0 || peers[i].port <= 1) continue;
        uint8_t f = flags ? (flags[i] & PEXF_ENCRYPT) : 0;
        int dup = -1;
        for (int j = 0; j < ctx->pool_count; j++) {
            if (ctx->pool[j].addr.ip == peers[i].ip &&
                ctx->pool[j].addr.port == peers[i].port) {
                dup = j;
                break;
            }
        }
        if (dup >= 0) {
            if (prio > ctx->pool[dup].prio) ctx->pool[dup].prio = prio;
            if (f) ctx->pool[dup].flags |= f;
            continue;
        }
        if (ctx->pool_count < META_MAX_PEERS) {
            meta_pool_entry *e = &ctx->pool[ctx->pool_count++];
            memset(e, 0, sizeof(*e));
            e->addr = peers[i];
            e->prio = prio;
            e->flags = f;
        }
    }
    torrent_meta_peers_total = ctx->pool_count;
}

static void pool_add(meta_ctx *ctx, const peer_addr *peers, int n,
                     uint8_t prio, const uint8_t *flags) {
    mutexLock(&ctx->mu);
    if (!ctx->done && !ctx->stop_disc) {
        pool_add_locked(ctx, peers, n, prio, flags);
    }
    mutexUnlock(&ctx->mu);
}

static void tracker_peer_handler(void *vctx, const peer_addr *peers, int n) {
    meta_ctx *ctx = (meta_ctx *)vctx;
    if (!ctx || ctx->done || ctx->stop_disc || mf_cancelled(ctx)) return;
    pool_add(ctx, peers, n, 2, NULL);
}

static void dht_peer_handler(void *vctx, const peer_addr *peers, int n) {
    meta_ctx *ctx = (meta_ctx *)vctx;
    if (!ctx || ctx->done || ctx->stop_disc || mf_cancelled(ctx)) return;
    pool_add(ctx, peers, n, 0, NULL);
}

static void tracker_disc_main(void *arg) {
    meta_ctx *ctx = (meta_ctx *)arg;
    char e[128];
    for (int round = 0; round < 3 && !ctx->done && !ctx->stop_disc && !mf_cancelled(ctx); round++) {
        if (round > 0) {
            int steps = (round == 1) ? 80 : 280;  /* 4 s before round 2, 14 s before round 3 */
            for (int k = 0; k < steps && !ctx->done && !ctx->stop_disc && !mf_cancelled(ctx); k++)
                svcSleepThread(50000000ULL);
        }
        if (round == 2) {
            mutexLock(&ctx->mu);
            bool skip_round = ctx->asm_init || ctx->pool_count >= META_MAX_PEERS;
            mutexUnlock(&ctx->mu);
            if (skip_round) break;
        }
        if (ctx->done || ctx->stop_disc || mf_cancelled(ctx)) break;
        torrent_announce_cb(ctx->announce_stub, tracker_peer_handler, ctx,
                            &ctx->stop_disc, e, sizeof(e));
    }
}

static void asm_reset(meta_ctx *ctx) {
    free(ctx->asm_buf);
    free(ctx->piece_state);
    free(ctx->piece_req_ms);
    ctx->asm_buf = NULL;
    ctx->piece_state = NULL;
    ctx->piece_req_ms = NULL;
    ctx->asm_init = false;
    ctx->asm_size = 0;
    ctx->npieces = 0;
    ctx->done_pieces = 0;
}

static bool asm_ensure(meta_ctx *ctx, int64_t meta_size) {
    if (ctx->asm_init) return (int64_t)ctx->asm_size == meta_size;
    if (meta_size <= 0 || meta_size > META_MAX_SIZE) return false;
    int np = (int)((meta_size + META_PIECE_LEN - 1) / META_PIECE_LEN);
    if (np <= 0) return false;
    uint8_t *buf = malloc((size_t)meta_size);
    uint8_t *st  = calloc((size_t)np, 1);
    uint64_t *ts = calloc((size_t)np, sizeof(uint64_t));
    if (!buf || !st || !ts) {
        free(buf); free(st); free(ts);
        return false;
    }
    ctx->asm_buf      = buf;
    ctx->piece_state  = st;
    ctx->piece_req_ms = ts;
    ctx->asm_size     = (size_t)meta_size;
    ctx->npieces      = np;
    ctx->done_pieces  = 0;
    ctx->asm_init     = true;
    return true;
}

static void sess_release_requests(meta_ctx *ctx, meta_sess *s) {
    for (int i = 0; i < s->req_cnt; i++) {
        int pc = s->req_pieces[i];
        if (ctx->asm_init && pc >= 0 && pc < ctx->npieces &&
            ctx->piece_state[pc] == PIECE_INFLIGHT) {
            ctx->piece_state[pc] = PIECE_NEEDED;
        }
    }
    s->req_cnt = 0;
}

static void sess_drop_req_piece(meta_sess *s, int piece) {
    for (int i = 0; i < s->req_cnt; i++) {
        if (s->req_pieces[i] == piece) {
            s->req_pieces[i] = s->req_pieces[s->req_cnt - 1];
            s->req_cnt--;
            return;
        }
    }
}

static void sess_close(meta_ctx *ctx, meta_sess *s, uint64_t now_ms, const char *why) {
    if (!s->active) return;
    if (why && why[0]) snprintf(s->close_why, sizeof(s->close_why), "%s", why);

    sess_release_requests(ctx, s);

    bool had_bt_hs = s->nb.handshaked;
    bool tcp_up    = s->tcp_connected;
    bool was_mse   = s->use_mse;
    bool was_utp   = s->use_utp;
    bool pre_hs_to = s->pre_hs_timeout;

    if (!ctx->done && why != NULL && !had_bt_hs && tcp_up) {
        if (pre_hs_to) ctx->stat_hs_to++;
        else ctx->stat_no_hs++;
    }

    if (s->nb.sock >= 0 || s->nb.utp != NULL) {
        peer_nb_free(&s->nb);
    }

    if (s->pidx >= 0) {
        mutexLock(&ctx->mu);
        meta_pool_entry *p = &ctx->pool[s->pidx];
        p->busy = false;
        if (!ctx->done) {
            if (!tcp_up && !was_utp) {
                /* TCP dial never connected (refused / timeout / NAT): schedule uTP retry */
                if (!p->tried_utp && utp_nb_fd() >= 0) {
                    p->need_utp = true;
                    p->next_try_ms = now_ms;
                }
            } else if (!had_bt_hs && tcp_up) {
                /* Connected at transport layer, closed before BT handshake completed */
                if (!was_mse && !p->tried_mse) {
                    /* Likely peer requiring MSE/PE encryption */
                    p->need_mse = true;
                    uint8_t pr = pre_hs_to ? 1 : 2;
                    if (pr > p->mse_prio) p->mse_prio = pr;
                    p->next_try_ms = now_ms;
                } else if (was_mse && !p->tried_plain) {
                    /* Peer rejected MSE / sent plaintext BT handshake (or timed out) */
                    p->need_plain = true;
                    p->next_try_ms = now_ms;
                }
                /* Also schedule uTP fallback behind TUN/VPN or after TCP MSE failure */
                if (!was_utp && !p->tried_utp && utp_nb_fd() >= 0 &&
                    (pre_hs_to || was_mse || p->tried_mse)) {
                    p->need_utp = true;
                    p->next_try_ms = now_ms;
                }
            }
            if (s->close_why[0]) {
                snprintf(torrent_meta_last_err, sizeof(torrent_meta_last_err),
                         "%s", s->close_why);
            }
        }
        mutexUnlock(&ctx->mu);
    }

    memset(s, 0, sizeof(*s));
    s->nb.sock = -1;
    s->pidx = -1;
}

static void sess_queue_initial_msgs(meta_ctx *ctx, meta_sess *s) {
    if (s->ext_sent) return;
    if (s->nb.ext_ok) {
        peer_nb_queue_ext_handshake_ex(&s->nb, torrent_announce_port(),
                                       s->addr.ip, false, true);
    }
    if (s->nb.dht_ok) {
        int dp = dht_bg_port();
        if (dp > 0 && dp < 65536) {
            uint8_t port_be[2] = { (uint8_t)(dp >> 8), (uint8_t)(dp & 0xff) };
            peer_nb_queue(&s->nb, MSG_PORT, port_be, 2);
        }
    }
    s->ext_sent = true;
    s->ext_hs_deadline_ms = mf_now_ms() + META_EXT_HS_WAIT_MS;
    if (peer_nb_tx_pending(&s->nb)) peer_nb_flush(&s->nb);
    (void)ctx;
}

static void sess_request_more(meta_ctx *ctx, meta_sess *s, uint64_t now_ms) {
    if (!ctx->asm_init || s->meta_id == 0 || s->meta_rejected) return;
    if (s->meta_size != (int64_t)ctx->asm_size) return;

    while (s->req_cnt < META_REQ_WINDOW) {
        int pick = -1;
        for (int i = 0; i < ctx->npieces; i++) {
            if (ctx->piece_state[i] == PIECE_NEEDED) {
                pick = i;
                break;
            }
        }
        if (pick < 0) break;

        char req[64];
        int rlen = snprintf(req, sizeof(req), "d8:msg_typei0e5:piecei%dee", pick);
        if (rlen <= 0 || peer_nb_queue_ext(&s->nb, s->meta_id, req, (uint32_t)rlen) != 0)
            break;

        ctx->piece_state[pick] = PIECE_INFLIGHT;
        ctx->piece_req_ms[pick] = now_ms;
        s->req_pieces[s->req_cnt++] = pick;
    }
    if (peer_nb_tx_pending(&s->nb)) peer_nb_flush(&s->nb);
}

static void handle_pex(meta_ctx *ctx, const uint8_t *pl, uint32_t plen) {
    if (!pl || plen < 2) return;
    be_node *d = be_parse((const char *)pl, plen);
    if (!d || d->type != BE_DICT) { be_free(d); return; }

    be_node *added = be_dict_get(d, "added");
    be_node *addedf = be_dict_get(d, "added.f");
    if (added && added->type == BE_STR && added->str.len >= 6) {
        size_t cnt = added->str.len / 6;
        if (cnt > 64) cnt = 64;
        peer_addr addrs[64];
        uint8_t fl[64];
        int valid = 0;
        const uint8_t *p = (const uint8_t *)added->str.ptr;
        const uint8_t *fp = (addedf && addedf->type == BE_STR)
                                ? (const uint8_t *)addedf->str.ptr : NULL;
        size_t flen = (addedf && addedf->type == BE_STR) ? addedf->str.len : 0;
        for (size_t i = 0; i < cnt; i++) {
            uint32_t ip;
            uint16_t port;
            memcpy(&ip, p + i * 6, 4);
            port = (uint16_t)((p[i * 6 + 4] << 8) | p[i * 6 + 5]);
            if (ip != 0 && port > 1) {
                addrs[valid].ip = ip;
                addrs[valid].port = port;
                fl[valid] = (fp && i < flen) ? (fp[i] & PEXF_ENCRYPT) : 0;
                valid++;
            }
        }
        if (valid > 0) {
            pool_add(ctx, addrs, valid, 1, fl);
        }
    }
    be_free(d);
}

static void check_meta_complete(meta_ctx *ctx, uint64_t now_ms) {
    if (!ctx->asm_init || ctx->done_pieces < ctx->npieces) return;

    uint8_t digest[20];
    mbedtls_sha1(ctx->asm_buf, ctx->asm_size, digest);
    if (memcmp(digest, ctx->info_hash, 20) == 0) {
        ctx->done = true;
        return;
    }

    /* Rare poisoned piece: ban contributing peers and restart assembly */
    mf_note_err(ctx, "invalid metadata hash", false);
    for (int i = 0; i < ctx->max_sess; i++) {
        meta_sess *s = &ctx->S[i];
        if (!s->active) continue;
        if (s->contributed) {
            if (s->pidx >= 0) {
                mutexLock(&ctx->mu);
                ctx->pool[s->pidx].bad = true;
                mutexUnlock(&ctx->mu);
            }
            sess_close(ctx, s, now_ms, "bad metadata hash");
        }
    }
    asm_reset(ctx);
}

static void handle_ext_msg(meta_ctx *ctx, meta_sess *s, const uint8_t *pl,
                           uint32_t plen, uint64_t now_ms) {
    if (!pl || plen < 1) return;
    uint8_t ext_id = pl[0];
    const uint8_t *body = pl + 1;
    uint32_t blen = plen - 1;

    if (ext_id == 0) {
        peer_ext_info ei;
        if (!peer_parse_ext_handshake(body, blen, &ei)) {
            sess_close(ctx, s, now_ms, "bad ext handshake");
            return;
        }
        s->ext_received = true;
        ctx->stat_ext_ok++;
        s->pex_id = ei.pex_id;
        if (ei.meta_id > 0 && ei.meta_size > 0 && ei.meta_size <= META_MAX_SIZE) {
            s->meta_id = ei.meta_id;
            s->meta_size = ei.meta_size;
            if (asm_ensure(ctx, ei.meta_size)) {
                sess_request_more(ctx, s, now_ms);
            } else if (ctx->asm_init && (int64_t)ctx->asm_size != ei.meta_size) {
                s->meta_rejected = true;
            }
        }
        if (s->meta_id == 0 || s->meta_rejected) {
            if (s->pex_id != 0) {
                /* No metadata here; wait briefly for ut_pex peers */
                s->pex_until_ms = now_ms + META_PEX_LINGER_MS;
            } else {
                sess_close(ctx, s, now_ms, "peer does not share metadata");
            }
        }
        return;
    }

    if (ext_id == EXT_ID_PEX) {
        handle_pex(ctx, body, blen);
        return;
    }

    if (ext_id == EXT_ID_METADATA) {
        be_node *d = be_parse((const char *)body, blen);
        if (!d || d->type != BE_DICT) { be_free(d); return; }
        be_node *mt = be_dict_get(d, "msg_type");
        be_node *pc = be_dict_get(d, "piece");
        int64_t msg_type = (mt && mt->type == BE_INT) ? mt->i : -1;
        int idx = (pc && pc->type == BE_INT) ? (int)pc->i : -1;
        size_t hdr_len = d->rawlen;
        be_free(d);

        if (msg_type == 1) { /* DATA */
            sess_drop_req_piece(s, idx);
            if (!ctx->asm_init || idx < 0 || idx >= ctx->npieces) return;
            size_t expect = (idx == ctx->npieces - 1)
                                ? (ctx->asm_size - (size_t)idx * META_PIECE_LEN)
                                : META_PIECE_LEN;
            if (blen < hdr_len + expect) return;

            if (ctx->piece_state[idx] != PIECE_DONE) {
                memcpy(ctx->asm_buf + (size_t)idx * META_PIECE_LEN, body + hdr_len, expect);
                ctx->piece_state[idx] = PIECE_DONE;
                ctx->done_pieces++;
                ctx->last_progress_ms = now_ms;
                s->contributed = true;
                if (s->pidx >= 0) {
                    mutexLock(&ctx->mu);
                    ctx->pool[s->pidx].served_meta = true;
                    mutexUnlock(&ctx->mu);
                }
            }
            check_meta_complete(ctx, now_ms);
            if (!ctx->done) sess_request_more(ctx, s, now_ms);
        } else if (msg_type == 2) { /* REJECT */
            sess_release_requests(ctx, s);
            s->meta_rejected = true;
            if (s->pex_id != 0 && s->pex_until_ms == 0) {
                s->pex_until_ms = now_ms + 2000;
            } else {
                sess_close(ctx, s, now_ms, "metadata rejected");
            }
        }
    }
}

static ssize_t sess_process_rx(meta_ctx *ctx, meta_sess *s, uint64_t now_ms) {
    ssize_t got = peer_nb_pump_rx(&s->nb);
    if (got == -2) {
        s->nb.mse_failed = true;
        sess_close(ctx, s, now_ms, "mse fallback");
        return -1;
    }
    if (got < 0) {
        sess_close(ctx, s, now_ms, s->nb.handshaked ? "peer closed" : "no handshake");
        return -1;
    }
    if (got > 0) s->last_rx_ms = now_ms;

    if (!s->nb.handshaked) {
        if (s->nb.mse_want && !s->nb.mse_active) {
            if (peer_nb_tx_pending(&s->nb) && peer_nb_flush(&s->nb) != 0) {
                sess_close(ctx, s, now_ms, "mse tx fail");
                return -1;
            }
            return got;
        }
        int hs = peer_nb_recv_handshake(&s->nb, ctx->info_hash);
        if (hs < 0) {
            sess_close(ctx, s, now_ms, "info_hash mismatch");
            return -1;
        }
        if (hs == 0) {
            if (peer_nb_tx_pending(&s->nb) && peer_nb_flush(&s->nb) != 0) {
                sess_close(ctx, s, now_ms, "hs tx fail");
                return -1;
            }
            return got;
        }
        ctx->stat_bt_hs++;
        if (!s->nb.ext_ok) {
            sess_close(ctx, s, now_ms, "peer without extensions");
            return -1;
        }
        sess_queue_initial_msgs(ctx, s);
    }

    for (int msgs = 0; msgs < 64 && s->active && !ctx->done; msgs++) {
        uint8_t id;
        uint8_t *pl;
        uint32_t plen;
        int r = peer_nb_next(&s->nb, &id, &pl, &plen);
        if (r < 0) {
            sess_close(ctx, s, now_ms, "bad framing");
            return -1;
        }
        if (r == 0) break;

        if (id == MSG_PORT && plen == 2) {
            uint16_t dport = (uint16_t)((pl[0] << 8) | pl[1]);
            dht_bg_add_node(s->addr.ip, dport);
        } else if (id == MSG_EXTENDED) {
            handle_ext_msg(ctx, s, pl, plen, now_ms);
            if (!s->active) return -1;
        }
    }
    return got;
}

static void attach_incoming_item(meta_ctx *ctx, const meta_inc_item *it, uint64_t now_ms) {
    int sid = -1;
    for (int i = 0; i < ctx->max_sess; i++) {
        if (!ctx->S[i].active) { sid = i; break; }
    }
    if (sid < 0) {
        if (it->utp) utp_nb_close((utp_nb_sess *)it->utp);
        else close(it->fd);
        return;
    }

    uint32_t rip = it->ip;
    uint16_t rport = it->port;
    if (!it->utp && rip == 0) {
        struct sockaddr_in psa;
        socklen_t psl = sizeof(psa);
        if (getpeername(it->fd, (struct sockaddr *)&psa, &psl) == 0) {
            rip = psa.sin_addr.s_addr;
            rport = ntohs(psa.sin_port);
        }
    }

    meta_sess *s = &ctx->S[sid];
    memset(s, 0, sizeof(*s));
    s->nb.sock = -1;
    s->pidx = -1;

    int rc = it->utp
        ? peer_nb_init_utp_cap(&s->nb, it->utp, 0, META_RX_CAP)
        : peer_nb_init_cap(&s->nb, it->fd, 0, META_RX_CAP);
    if (rc != 0) {
        if (it->utp) utp_nb_close((utp_nb_sess *)it->utp);
        else close(it->fd);
        return;
    }

    s->active = true;
    s->connecting = false;
    s->tcp_connected = true;
    s->incoming = true;
    s->use_utp = (it->utp != NULL);
    s->addr.ip = rip;
    s->addr.port = rport;
    s->started_ms = now_ms;
    s->connected_ms = now_ms;
    s->last_rx_ms = now_ms;

    s->nb.handshaked = true;
    s->nb.ext_ok = (it->hs[25] & 0x10) != 0;
    s->nb.dht_ok = (it->hs[27] & 0x01) != 0;
    ctx->stat_tcp_ok++;
    ctx->stat_bt_hs++;

    if (!s->nb.ext_ok) {
        sess_close(ctx, s, now_ms, "incoming peer without extensions");
        return;
    }

    peer_nb_send_handshake(&s->nb, ctx->info_hash, ctx->peer_id);
    sess_queue_initial_msgs(ctx, s);
    torrent_meta_connected++;
    sess_process_rx(ctx, s, now_ms);
}

static void drain_incoming(meta_ctx *ctx, uint64_t now_ms) {
    /* Accept incoming uTP connections if uTP is initialized */
    if (utp_nb_fd() >= 0) {
        for (int k = 0; k < 4 && !ctx->done; k++) {
            uint8_t hs[68];
            uint32_t ip = 0;
            uint16_t port = 0;
            utp_nb_sess *us = utp_nb_accept(hs, &ip, &port);
            if (!us) break;
            bool ok = false;
            if (hs[0] == 19 && memcmp(hs + 1, "BitTorrent protocol", 19) == 0) {
                if (memcmp(hs + 28, ctx->info_hash, 20) == 0) {
                    ok = inc_push(ctx, -1, us, ip, port, hs);
                } else if (g_utp_router) {
                    ok = g_utp_router(hs + 28, us, hs, ip, port);
                }
            }
            if (!ok) utp_nb_close(us);
        }
    }

    for (;;) {
        meta_inc_item it;
        bool have = false;
        mutexLock(&ctx->mu);
        if (ctx->inc_n > 0) {
            it = ctx->inc[--ctx->inc_n];
            have = true;
        }
        mutexUnlock(&ctx->mu);
        if (!have) break;
        if (ctx->done || mf_cancelled(ctx)) {
            if (it.utp) utp_nb_close((utp_nb_sess *)it.utp);
            else close(it.fd);
            continue;
        }
        attach_incoming_item(ctx, &it, now_ms);
    }
}

static int take_next_peer(meta_ctx *ctx, uint64_t now_ms, int active_mse, int utp_dialing,
                          int *out_mode, peer_addr *out_addr) {
    int pick = -1;
    int pick_mode = DIAL_PLAIN;
    int mse_cap = ctx->max_sess / 2 > 0 ? ctx->max_sess / 2 : 1;

    mutexLock(&ctx->mu);
    /* Pass 0a: high-priority MSE retry for peers that actively closed/RST plaintext */
    for (int i = 0; i < ctx->pool_count && active_mse < mse_cap; i++) {
        meta_pool_entry *e = &ctx->pool[i];
        if (e->busy || e->bad || !e->need_mse || e->tried_mse || e->mse_prio < 2) continue;
        if (e->next_try_ms <= now_ms) {
            pick = i;
            pick_mode = DIAL_MSE;
            break;
        }
    }
    /* Pass 0b: interleaved uTP retry so NAT/TUN fallbacks don't wait behind all TCP */
    if (pick < 0 && utp_nb_fd() >= 0 && utp_dialing < META_UTP_MAX_DIAL &&
        (ctx->dial_seq & 3) == 3) {
        for (int i = 0; i < ctx->pool_count; i++) {
            meta_pool_entry *e = &ctx->pool[i];
            if (e->busy || e->bad || !e->need_utp || e->tried_utp) continue;
            if (e->next_try_ms <= now_ms) {
                pick = i;
                pick_mode = DIAL_UTP;
                break;
            }
        }
    }
    /* Pass 1: completely untried peer with highest priority */
    if (pick < 0) {
        int best_prio = -1;
        for (int i = 0; i < ctx->pool_count; i++) {
            meta_pool_entry *e = &ctx->pool[i];
            if (e->busy || e->bad || e->tried_plain || e->tried_mse || e->tried_utp) continue;
            if ((int)e->prio > best_prio) {
                best_prio = (int)e->prio;
                pick = i;
                pick_mode = (e->flags & PEXF_ENCRYPT) ? DIAL_MSE : DIAL_PLAIN;
            }
        }
    }
    /* Pass 2: remaining MSE fallback (prefer active close over silent timeout) */
    if (pick < 0) {
        int best_mse_prio = -1;
        for (int i = 0; i < ctx->pool_count; i++) {
            meta_pool_entry *e = &ctx->pool[i];
            if (e->busy || e->bad || !e->need_mse || e->tried_mse) continue;
            if (e->next_try_ms <= now_ms && (int)e->mse_prio > best_mse_prio) {
                best_mse_prio = (int)e->mse_prio;
                pick = i;
                pick_mode = DIAL_MSE;
            }
        }
    }
    /* Pass 3: plaintext fallback after MSE rejected */
    if (pick < 0) {
        for (int i = 0; i < ctx->pool_count; i++) {
            meta_pool_entry *e = &ctx->pool[i];
            if (e->busy || e->bad || !e->need_plain || e->tried_plain) continue;
            if (e->next_try_ms <= now_ms) {
                pick = i;
                pick_mode = DIAL_PLAIN;
                break;
            }
        }
    }
    /* Pass 4: uTP fallback for NAT / TCP-unreachable peers */
    if (pick < 0 && utp_nb_fd() >= 0 && utp_dialing < META_UTP_MAX_DIAL) {
        for (int i = 0; i < ctx->pool_count; i++) {
            meta_pool_entry *e = &ctx->pool[i];
            if (e->busy || e->bad || !e->need_utp || e->tried_utp) continue;
            if (e->next_try_ms <= now_ms) {
                pick = i;
                pick_mode = DIAL_UTP;
                break;
            }
        }
    }

    if (pick >= 0) {
        meta_pool_entry *e = &ctx->pool[pick];
        e->busy = true;
        if (!e->counted_tried) {
            e->counted_tried = true;
            torrent_meta_peers_tried++;
        }
        if (pick_mode == DIAL_PLAIN) {
            e->tried_plain = true;
            e->need_plain = false;
        } else if (pick_mode == DIAL_MSE) {
            e->tried_mse = true;
            e->need_mse = false;
        } else {
            e->tried_utp = true;
            e->need_utp = false;
        }
        ctx->dial_seq++;
        ctx->stat_dial[pick_mode]++;
        *out_addr = e->addr;
        *out_mode = pick_mode;
    }
    mutexUnlock(&ctx->mu);
    return pick;
}

static bool dial_peer(meta_ctx *ctx, int sid, int pidx, peer_addr pa, int mode, uint64_t now_ms) {
    meta_sess *s = &ctx->S[sid];
    memset(s, 0, sizeof(*s));
    s->nb.sock = -1;
    s->pidx = pidx;
    s->addr = pa;
    s->started_ms = now_ms;
    s->last_rx_ms = now_ms;

    if (mode == DIAL_UTP) {
        utp_nb_sess *us = utp_nb_connect(pa.ip, pa.port);
        if (!us || peer_nb_init_utp_cap(&s->nb, us, 0, META_RX_CAP) != 0) {
            if (us) utp_nb_close(us);
            mutexLock(&ctx->mu);
            ctx->pool[pidx].busy = false;
            mutexUnlock(&ctx->mu);
            memset(s, 0, sizeof(*s));
            s->nb.sock = -1;
            s->pidx = -1;
            mf_note_err(ctx, "utp connect failed", false);
            return false;
        }
        s->active = true;
        s->connecting = true;
        s->use_utp = true;
        s->use_mse = (ctx->pool[pidx].flags & PEXF_ENCRYPT) != 0;
        s->conn_deadline_ms = now_ms + META_UTP_CONN_MS;
        return true;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        int cerr = errno;
        bool is_local = (cerr == ENOBUFS || cerr == ENOMEM || cerr == EMFILE || cerr == ENFILE);
        mutexLock(&ctx->mu);
        ctx->pool[pidx].busy = false;
        if (is_local) {
            /* Do not burn peer's dial status on transient socket exhaustion */
            if (mode == DIAL_PLAIN) ctx->pool[pidx].tried_plain = false;
            else ctx->pool[pidx].tried_mse = false;
        }
        mutexUnlock(&ctx->mu);
        memset(s, 0, sizeof(*s));
        s->nb.sock = -1;
        s->pidx = -1;
        mf_note_err(ctx, "socket", false);
        return false;
    }

#ifdef __SWITCH__
    int rcvbuf = 32 * 1024;
    int sndbuf = 16 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#endif
    int fl = fcntl(sock, F_GETFL, 0);
    if (fl >= 0) fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = pa.ip;
    sa.sin_port = htons(pa.port);

    int rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        int cerr = errno;
        close(sock);
        bool is_local = (cerr == ENOBUFS || cerr == ENOMEM || cerr == EMFILE || cerr == ENFILE);
        mutexLock(&ctx->mu);
        ctx->pool[pidx].busy = false;
        if (is_local) {
            if (mode == DIAL_PLAIN) ctx->pool[pidx].tried_plain = false;
            else ctx->pool[pidx].tried_mse = false;
        } else if (utp_nb_fd() >= 0 && !ctx->pool[pidx].tried_utp) {
            ctx->pool[pidx].need_utp = true;
            ctx->pool[pidx].next_try_ms = now_ms;
        }
        mutexUnlock(&ctx->mu);
        memset(s, 0, sizeof(*s));
        s->nb.sock = -1;
        s->pidx = -1;
        mf_note_err(ctx, "connection refused", false);
        return !is_local;
    }

    s->active = true;
    s->connecting = true;
    s->use_mse = (mode == DIAL_MSE);
    s->nb.sock = sock;
    s->conn_deadline_ms = now_ms + META_TCP_CONN_MS;

    if (rc == 0) {
        /* Immediate loopback connect: finalize right away */
        if (peer_nb_init_cap(&s->nb, sock, 0, META_RX_CAP) != 0) {
            close(sock);
            s->nb.sock = -1;
            sess_close(ctx, s, now_ms, "oom");
            return false;
        }
        s->connecting = false;
        s->tcp_connected = true;
        s->connected_ms = now_ms;
        ctx->stat_tcp_ok++;
        s->nb.mse_want = s->use_mse;
        torrent_meta_connected++;
        peer_nb_send_handshake(&s->nb, ctx->info_hash, ctx->peer_id);
        peer_nb_flush(&s->nb);
    }
    return true;
}

static void tcp_connected(meta_ctx *ctx, meta_sess *s, uint64_t now_ms) {
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (getsockopt(s->nb.sock, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
        if (soerr == ECONNRESET || soerr == ECONNABORTED
#ifdef _WIN32
            || soerr == WSAECONNRESET || soerr == WSAECONNABORTED
#endif
        ) {
            s->tcp_connected = true;
            s->connected_ms = now_ms;
        }
        mf_note_err(ctx, "connection refused", false);
        sess_close(ctx, s, now_ms, "connection refused");
        return;
    }
    int sock = s->nb.sock;
    if (peer_nb_init_cap(&s->nb, sock, 0, META_RX_CAP) != 0) {
        close(sock);
        s->nb.sock = -1;
        sess_close(ctx, s, now_ms, "oom");
        return;
    }
    s->connecting = false;
    s->tcp_connected = true;
    s->connected_ms = now_ms;
    s->last_rx_ms = now_ms;
    ctx->stat_tcp_ok++;
    s->nb.mse_want = s->use_mse;
    torrent_meta_connected++;
    peer_nb_send_handshake(&s->nb, ctx->info_hash, ctx->peer_id);
    peer_nb_flush(&s->nb);
}

int meta_fetch_run(const meta_fetch_opts *opts,
                   uint8_t **out_meta, size_t *out_len,
                   peer_addr *out_peers, int max_peers, int *out_peer_n,
                   char *err, size_t errlen) {
    if (out_meta) *out_meta = NULL;
    if (out_len) *out_len = 0;
    if (out_peer_n) *out_peer_n = 0;
    if (!opts || !opts->info_hash || !opts->peer_id || !out_meta || !out_len) {
        mf_set_err(err, errlen, "invalid args");
        return -1;
    }

    meta_ctx *ctx = calloc(1, sizeof(meta_ctx));
    if (!ctx) {
        mf_set_err(err, errlen, "out of memory");
        return -1;
    }
    mutexInit(&ctx->mu);
    memcpy(ctx->info_hash, opts->info_hash, 20);
    memcpy(ctx->peer_id, opts->peer_id, 20);
    ctx->cancel = opts->cancel;
    ctx->announce_stub = opts->announce_stub;
    ctx->max_sess = opts->max_sess > 0
                        ? opts->max_sess
                        : (g_low_concurrency ? 8 : META_DEFAULT_SESS);
    if (ctx->max_sess > META_HARD_MAX_SESS) ctx->max_sess = META_HARD_MAX_SESS;
    for (int i = 0; i < META_HARD_MAX_SESS; i++) {
        ctx->S[i].nb.sock = -1;
        ctx->S[i].pidx = -1;
    }

    if (opts->seed_peers && opts->seed_count > 0) {
        pool_add(ctx, opts->seed_peers, opts->seed_count, 2, NULL);
    }

    reg_add(ctx);

    if (opts->use_dht) {
        dht_bg_hungry_push();
        dht_background_add(ctx->info_hash, dht_peer_handler, ctx);
    }
    if (ctx->announce_stub) {
        if (threadCreate(&ctx->disc_thread, tracker_disc_main, ctx, NULL, 0x8000,
                         TSNX_THREAD_ARGS(TSNX_CORE_NET, TSNX_PRIO_NET_BG)) == 0) {
            if (threadStart(&ctx->disc_thread) == 0) {
                ctx->disc_started = true;
            } else {
                threadClose(&ctx->disc_thread);
            }
        }
        if (!ctx->disc_started) {
            char e[128];
            torrent_announce_cb(ctx->announce_stub, tracker_peer_handler, ctx,
                                ctx->cancel, e, sizeof(e));
        }
    }

    uint64_t start_ms = mf_now_ms();
    int budget = opts->budget_ms > 0 ? opts->budget_ms : META_DEFAULT_BUDGET_MS;
    uint64_t base_deadline = start_ms + (uint64_t)budget;
    uint64_t max_deadline  = start_ms + (uint64_t)budget + META_EXTRA_PROGRESS_MS;

    while (!ctx->done && !mf_cancelled(ctx)) {
        uint64_t now_ms = mf_now_ms();
        uint64_t eff_deadline = base_deadline;
        if (ctx->asm_init && ctx->done_pieces > 0 &&
            now_ms - ctx->last_progress_ms < 5000) {
            uint64_t ext = ctx->last_progress_ms + 8000;
            if (ext > eff_deadline) eff_deadline = ext;
            if (eff_deadline > max_deadline) eff_deadline = max_deadline;
        }
        if (now_ms >= eff_deadline) break;

        drain_incoming(ctx, now_ms);
        if (ctx->done) break;

        /* Dial available pool entries up to max_sess */
        int active_cnt = 0;
        int active_mse = 0;
        int utp_dialing = 0;
        for (int i = 0; i < ctx->max_sess; i++) {
            if (!ctx->S[i].active) continue;
            active_cnt++;
            if (ctx->S[i].use_mse) active_mse++;
            if (ctx->S[i].connecting && ctx->S[i].use_utp) utp_dialing++;
        }

        while (active_cnt < ctx->max_sess && !ctx->done && !mf_cancelled(ctx)) {
            int sid = -1;
            for (int i = 0; i < ctx->max_sess; i++) {
                if (!ctx->S[i].active) { sid = i; break; }
            }
            if (sid < 0) break;

            int mode = DIAL_PLAIN;
            peer_addr pa = {0, 0};
            int pidx = take_next_peer(ctx, now_ms, active_mse, utp_dialing, &mode, &pa);
            if (pidx < 0) break;

            if (!dial_peer(ctx, sid, pidx, pa, mode, now_ms)) {
                /* Do not spin on local socket errors */
                if (!ctx->S[sid].active && errno == ENOBUFS) break;
                continue;
            }
            active_cnt++;
            if (mode == DIAL_MSE) active_mse++;
            if (mode == DIAL_UTP) utp_dialing++;
        }
        if (mf_cancelled(ctx)) {
            ctx->stop_disc = true;
            break;
        }

        /* Re-expire stalled inflight piece requests & feed idle peers */
        if (ctx->asm_init && !ctx->done) {
            for (int i = 0; i < ctx->max_sess; i++) {
                meta_sess *s = &ctx->S[i];
                if (!s->active) continue;
                for (int r = s->req_cnt - 1; r >= 0; r--) {
                    int pc = s->req_pieces[r];
                    if (pc >= 0 && pc < ctx->npieces &&
                        ctx->piece_state[pc] == PIECE_INFLIGHT &&
                        now_ms - ctx->piece_req_ms[pc] > META_PIECE_REQ_MS) {
                        ctx->piece_state[pc] = PIECE_NEEDED;
                        s->req_pieces[r] = s->req_pieces[s->req_cnt - 1];
                        s->req_cnt--;
                    }
                }
                if (s->req_cnt < META_REQ_WINDOW && s->ext_received && !s->meta_rejected) {
                    sess_request_more(ctx, s, now_ms);
                }
            }
        }

        /* Seeded single-peer fast exit if peer has no more transports to attempt */
        if (opts->seed_count > 0 && !ctx->announce_stub && !opts->use_dht && active_cnt == 0) {
            bool any_pending = false;
            mutexLock(&ctx->mu);
            for (int i = 0; i < ctx->pool_count; i++) {
                meta_pool_entry *e = &ctx->pool[i];
                if (e->bad) continue;
                if ((!e->tried_plain && !e->tried_mse && !e->tried_utp) ||
                    (e->need_mse && !e->tried_mse) ||
                    (e->need_plain && !e->tried_plain) ||
                    (e->need_utp && !e->tried_utp)) {
                    any_pending = true;
                    break;
                }
            }
            mutexUnlock(&ctx->mu);
            if (!any_pending) break;
        }

        /* Build poll set */
        struct pollfd pfd[META_HARD_MAX_SESS + 1];
        int map[META_HARD_MAX_SESS + 1];
        int npfd = 0;
        for (int i = 0; i < ctx->max_sess; i++) {
            meta_sess *s = &ctx->S[i];
            if (!s->active || s->nb.sock < 0) continue;
            pfd[npfd].fd = s->nb.sock;
            pfd[npfd].events = s->connecting
                ? POLLOUT
                : (short)(POLLIN | (peer_nb_tx_pending(&s->nb) ? POLLOUT : 0));
            pfd[npfd].revents = 0;
            map[npfd] = i;
            npfd++;
        }
        int utp_fd = utp_nb_fd();
        if (utp_fd >= 0) {
            pfd[npfd].fd = utp_fd;
            pfd[npfd].events = POLLIN;
            pfd[npfd].revents = 0;
            map[npfd] = -1;
            npfd++;
        }

        if (npfd == 0) {
            svcSleepThread(25000000ULL);
        } else {
            int pr = poll(pfd, (nfds_t)npfd, 50);
            now_ms = mf_now_ms();
            bool utp_serviced = false;
            if (pr > 0) {
                for (int k = 0; k < npfd && !ctx->done; k++) {
                    if (!pfd[k].revents) continue;
                    if (map[k] == -1) {
                        utp_nb_service();
                        utp_serviced = true;
                        continue;
                    }
                    meta_sess *s = &ctx->S[map[k]];
                    if (!s->active) continue;
                    if (s->connecting) {
                        if (pfd[k].revents & (POLLOUT | POLLERR | POLLHUP)) {
                            tcp_connected(ctx, s, now_ms);
                        }
                        continue;
                    }
                    if (pfd[k].revents & POLLOUT) {
                        if (peer_nb_flush(&s->nb) != 0) {
                            sess_close(ctx, s, now_ms, "tx error");
                            continue;
                        }
                    }
                    if (pfd[k].revents & (POLLIN | POLLERR | POLLHUP)) {
                        ssize_t rx_n = sess_process_rx(ctx, s, now_ms);
                        if (s->active && rx_n == 0 && (pfd[k].revents & (POLLERR | POLLHUP))) {
                            sess_close(ctx, s, now_ms,
                                       (pfd[k].revents & POLLERR) ? "pollerr" : "pollhup");
                        }
                    }
                }
            }
            if (!utp_serviced && utp_fd >= 0) {
                utp_nb_service();
            }
        }

        now_ms = mf_now_ms();

        /* Pump active uTP sessions & enforce session timeouts */
        for (int i = 0; i < ctx->max_sess && !ctx->done; i++) {
            meta_sess *s = &ctx->S[i];
            if (!s->active) continue;

            if (s->use_utp) {
                if (s->connecting) {
                    int st = utp_nb_state((utp_nb_sess *)s->nb.utp);
                    if (st == 1) {
                        s->connecting = false;
                        s->tcp_connected = true;
                        s->connected_ms = now_ms;
                        s->last_rx_ms = now_ms;
                        ctx->stat_tcp_ok++;
                        s->nb.mse_want = s->use_mse;
                        torrent_meta_connected++;
                        peer_nb_send_handshake(&s->nb, ctx->info_hash, ctx->peer_id);
                        peer_nb_flush(&s->nb);
                    } else if (st < 0) {
                        sess_close(ctx, s, now_ms, "utp connect refused");
                        continue;
                    }
                }
                if (s->active && !s->connecting) {
                    sess_process_rx(ctx, s, now_ms);
                    if (s->active && peer_nb_tx_pending(&s->nb)) {
                        if (peer_nb_flush(&s->nb) != 0) {
                            sess_close(ctx, s, now_ms, "utp tx error");
                            continue;
                        }
                    }
                }
            }

            if (!s->active) continue;
            if (s->connecting && now_ms >= s->conn_deadline_ms) {
                sess_close(ctx, s, now_ms, "connect timeout");
            } else if (!s->connecting && !s->nb.handshaked &&
                       now_ms - s->connected_ms >= META_HS_WAIT_MS) {
                s->pre_hs_timeout = true;
                sess_close(ctx, s, now_ms, "handshake timeout");
            } else if (!s->connecting && s->ext_sent && !s->ext_received &&
                       now_ms >= s->ext_hs_deadline_ms) {
                sess_close(ctx, s, now_ms, "ext handshake timeout");
            } else if (!s->connecting && s->pex_until_ms > 0 && now_ms >= s->pex_until_ms) {
                sess_close(ctx, s, now_ms, "pex harvest done");
            } else if (!s->connecting && now_ms - s->last_rx_ms > 10000) {
                sess_close(ctx, s, now_ms, "idle timeout");
            }
        }
    }

    /* Tear down cleanly before releasing ctx */
    ctx->stop_disc = true;
    reg_remove(ctx);
    if (opts->use_dht) {
        dht_background_remove_ctx(ctx->info_hash, dht_peer_handler, ctx);
        dht_bg_hungry_pop();
    }
    if (ctx->disc_started) {
        threadWaitForExit(&ctx->disc_thread);
        threadClose(&ctx->disc_thread);
        ctx->disc_started = false;
    }

    uint64_t end_ms = mf_now_ms();
    for (int i = 0; i < ctx->max_sess; i++) {
        if (ctx->S[i].active) sess_close(ctx, &ctx->S[i], end_ms, NULL);
    }
    for (int i = 0; i < ctx->inc_n; i++) {
        if (ctx->inc[i].utp) utp_nb_close((utp_nb_sess *)ctx->inc[i].utp);
        else if (ctx->inc[i].fd >= 0) close(ctx->inc[i].fd);
    }
    ctx->inc_n = 0;

    engine_log(ENGINE_LOG_INFO,
               "[meta_fetch] done=%d cancel=%d %.1fs pool=%d dial=%d/%d/%d "
               "tcp=%d hs=%d ext=%d to=%d nohs=%d pieces=%d/%d",
               ctx->done ? 1 : 0, mf_cancelled(ctx) ? 1 : 0,
               (double)(end_ms - start_ms) / 1000.0,
               ctx->pool_count, ctx->stat_dial[0], ctx->stat_dial[1], ctx->stat_dial[2],
               ctx->stat_tcp_ok, ctx->stat_bt_hs, ctx->stat_ext_ok,
               ctx->stat_hs_to, ctx->stat_no_hs,
               ctx->done_pieces, ctx->npieces);

    int rc = -1;
    if (ctx->done && ctx->asm_buf && ctx->asm_size > 0) {
        *out_meta = ctx->asm_buf;
        *out_len = ctx->asm_size;
        ctx->asm_buf = NULL; /* transferred ownership to caller */

        if (out_peers && max_peers > 0 && out_peer_n) {
            int out_c = 0;
            /* Prioritize peers that actually served metadata, then others */
            for (int pass = 0; pass < 2 && out_c < max_peers; pass++) {
                for (int i = 0; i < ctx->pool_count && out_c < max_peers; i++) {
                    if (ctx->pool[i].bad) continue;
                    if ((pass == 0) != ctx->pool[i].served_meta) continue;
                    out_peers[out_c++] = ctx->pool[i].addr;
                }
            }
            *out_peer_n = out_c;
        }
        rc = 0;
    } else {
        if (mf_cancelled(ctx)) {
            mf_set_err(err, errlen, "metadata fetch cancelled");
        } else if (torrent_meta_last_err[0] && !opts->announce_stub && !opts->use_dht) {
            mf_set_err(err, errlen, torrent_meta_last_err);
        } else {
            mf_set_err(err, errlen, "no peer provided the metadata");
        }
    }

    asm_reset(ctx);
    free(ctx);
    return rc;
}

int meta_fetch_single_peer(peer_addr addr, const uint8_t info_hash[20],
                           const uint8_t peer_id[20],
                           const volatile bool *cancel,
                           uint8_t **out, size_t *out_len,
                           char *err, size_t errlen) {
    meta_fetch_opts opts = {0};
    opts.info_hash  = info_hash;
    opts.peer_id    = peer_id;
    opts.seed_peers = &addr;
    opts.seed_count = 1;
    opts.budget_ms  = META_SINGLE_BUDGET_MS;
    opts.max_sess   = 1;
    opts.cancel     = cancel;
    return meta_fetch_run(&opts, out, out_len, NULL, 0, NULL, err, errlen);
}
