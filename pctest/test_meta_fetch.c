#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <switch.h>
#include <mbedtls/bignum.h>
#include <mbedtls/sha1.h>

#include "meta_fetch.h"
#include "torrent_meta.h"
#include "bt_peer.h"
#include "mse_peer.h"
#include "bencode.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { tests_run++; printf("  " #name " ... "); fflush(stdout); test_##name(); printf("OK\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; return; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL(#cond); } while (0)
#define ASSERT_MSG(cond, m) do { if (!(cond)) { printf("FAIL: %s (%s)\n", #cond, (m)); tests_failed++; return; } } while (0)

static const uint8_t MSE_PRIME[MSE_DH_LEN] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC9,0x0F,0xDA,0xA2,
    0x21,0x68,0xC2,0x34,0xC4,0xC6,0x62,0x8B,0x80,0xDC,0x1C,0xD1,
    0x29,0x02,0x4E,0x08,0x8A,0x67,0xCC,0x74,0x02,0x0B,0xBE,0xA6,
    0x3B,0x13,0x9B,0x22,0x51,0x4A,0x08,0x79,0x8E,0x34,0x04,0xDD,
    0xEF,0x95,0x19,0xB3,0xCD,0x3A,0x43,0x1B,0x30,0x2B,0x0A,0x6D,
    0xF2,0x5F,0x14,0x37,0x4F,0xE1,0x35,0x6D,0x6D,0x51,0xC2,0x45,
    0xE4,0x85,0xB5,0x76,0x62,0x5E,0x7E,0xC6,0xF4,0x4C,0x42,0xE9,
    0xA6,0x3A,0x36,0x21,0x00,0x00,0x00,0x00,0x00,0x09,0x05,0x63
};

static uint64_t now_ms(void) {
    return (armGetSystemTick() * 1000ULL) / armGetSystemTickFreq();
}

static void dh_modexp_test(const uint8_t *base, size_t base_len,
                           const uint8_t *exp, uint8_t *out) {
    mbedtls_mpi b, e, p, r;
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_read_binary(&b, base, base_len);
    mbedtls_mpi_read_binary(&e, exp, MSE_DH_LEN);
    mbedtls_mpi_read_binary(&p, MSE_PRIME, MSE_DH_LEN);
    mbedtls_mpi_exp_mod(&r, &b, &e, &p, NULL);
    mbedtls_mpi_write_binary(&r, out, MSE_DH_LEN);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&r);
}

static void sha1_concat_test(const uint8_t *tag, size_t tag_len,
                             const uint8_t *d1, size_t l1,
                             const uint8_t *d2, size_t l2,
                             uint8_t out[20]) {
    mbedtls_sha1_context c;
    mbedtls_sha1_init(&c);
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, tag, tag_len);
    mbedtls_sha1_update(&c, d1, l1);
    if (d2) mbedtls_sha1_update(&c, d2, l2);
    mbedtls_sha1_finish(&c, out);
    mbedtls_sha1_free(&c);
}

static int send_all_opt(int fd, mse_rc4 *enc, const void *buf, size_t len, const volatile bool *stop) {
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t tmp[4096];
    size_t sent = 0;
    while (sent < len) {
        if (stop && *stop) return -1;
        size_t chunk = len - sent;
        if (enc && chunk > sizeof(tmp)) chunk = sizeof(tmp);
        const uint8_t *src = p + sent;
        if (enc) {
            mse_rc4_crypt(enc, src, tmp, chunk);
            src = tmp;
        }
        struct pollfd pfd = { (SOCKET)fd, POLLOUT, 0 };
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) continue;
        ssize_t n = send(fd, (const char *)src, chunk, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all_opt(int fd, mse_rc4 *dec, void *buf, size_t len, const volatile bool *stop) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        if (stop && *stop) return -1;
        struct pollfd pfd = { (SOCKET)fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) continue;
        ssize_t n = recv(fd, (char *)(p + got), len - got, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        if (dec) mse_rc4_crypt(dec, p + got, p + got, (size_t)n);
        got += (size_t)n;
    }
    return 0;
}

static int bind_loopback_listener(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
        close(fd);
        return -1;
    }
    if (out_port) *out_port = ntohs(sa.sin_port);
    return fd;
}

static uint8_t *build_test_info(size_t hash_entries, uint8_t ih[20], size_t *out_len) {
    size_t pieces_bytes = hash_entries * 20;
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "d6:lengthi1048576e4:name8:testgame12:piece lengthi16384e6:pieces%zu:",
                        pieces_bytes);
    size_t total = (size_t)hlen + pieces_bytes + 1;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;
    memcpy(buf, hdr, (size_t)hlen);
    for (size_t i = 0; i < pieces_bytes; i++) {
        buf[(size_t)hlen + i] = (uint8_t)((i * 31 + 7) & 0xFF);
    }
    buf[total - 1] = 'e';
    mbedtls_sha1(buf, total, ih);
    *out_len = total;
    return buf;
}

static void build_bt_hs(uint8_t hs[68], const uint8_t ih[20], const char *pid20) {
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    hs[25] = 0x10; // BEP-10 extensions
    memcpy(hs + 28, ih, 20);
    memcpy(hs + 48, pid20, 20);
}

static int send_bt_frame(int fd, mse_rc4 *enc, uint8_t msg_id, const void *payload,
                         size_t plen, const volatile bool *stop) {
    uint8_t hdr[5];
    uint32_t flen = htonl((uint32_t)(1 + plen));
    memcpy(hdr, &flen, 4);
    hdr[4] = msg_id;
    if (send_all_opt(fd, enc, hdr, 5, stop) != 0) return -1;
    if (plen > 0 && send_all_opt(fd, enc, payload, plen, stop) != 0) return -1;
    return 0;
}

static int mse_responder_handshake(int fd, const uint8_t ih[20],
                                   mse_rc4 *out_send, mse_rc4 *out_recv,
                                   uint8_t peer_hs[68], const volatile bool *stop) {
    uint8_t pubA[MSE_DH_LEN];
    if (recv_all_opt(fd, NULL, pubA, sizeof(pubA), stop) != 0) return -1;
    if (pubA[0] == 19 && memcmp(pubA + 1, "BitTorrent protocol", 19) == 0) {
        return -1; // plaintext, not MSE
    }

    uint8_t privB[MSE_DH_LEN];
    memset(privB, 0, MSE_DH_LEN - 20);
    randomGet(privB + MSE_DH_LEN - 20, 20);
    static const uint8_t two[1] = { 2 };
    uint8_t pubB[MSE_DH_LEN];
    dh_modexp_test(two, 1, privB, pubB);

    // Send pubB + 17 bytes non-empty padB to test VC resync.
    uint8_t init_reply[MSE_DH_LEN + 17];
    memcpy(init_reply, pubB, MSE_DH_LEN);
    for (int i = 0; i < 17; i++) init_reply[MSE_DH_LEN + i] = (uint8_t)(0x42 + i);
    if (send_all_opt(fd, NULL, init_reply, sizeof(init_reply), stop) != 0) return -1;

    uint8_t secret[MSE_DH_LEN];
    dh_modexp_test(pubA, MSE_DH_LEN, privB, secret);

    uint8_t r1[20], r2[20], r3[20], x23[20];
    sha1_concat_test((const uint8_t *)"req1", 4, secret, MSE_DH_LEN, NULL, 0, r1);
    sha1_concat_test((const uint8_t *)"req2", 4, ih, 20, NULL, 0, r2);
    sha1_concat_test((const uint8_t *)"req3", 4, secret, MSE_DH_LEN, NULL, 0, r3);
    for (int i = 0; i < 20; i++) x23[i] = (uint8_t)(r2[i] ^ r3[i]);

    uint8_t in_req[40];
    if (recv_all_opt(fd, NULL, in_req, 40, stop) != 0) return -1;
    if (memcmp(in_req, r1, 20) != 0 || memcmp(in_req + 20, x23, 20) != 0) return -1;

    uint8_t kA[20], kB[20];
    sha1_concat_test((const uint8_t *)"keyA", 4, secret, MSE_DH_LEN, ih, 20, kA);
    sha1_concat_test((const uint8_t *)"keyB", 4, secret, MSE_DH_LEN, ih, 20, kB);
    mse_rc4_init(out_recv, kA, 20);
    mse_rc4_skip(out_recv, 1024);
    mse_rc4_init(out_send, kB, 20);
    mse_rc4_skip(out_send, 1024);

    uint8_t vc_prov_pad[14];
    if (recv_all_opt(fd, out_recv, vc_prov_pad, sizeof(vc_prov_pad), stop) != 0) return -1;
    uint8_t zeros[8] = {0};
    if (memcmp(vc_prov_pad, zeros, 8) != 0) return -1;
    uint16_t padC = (uint16_t)(((uint16_t)vc_prov_pad[12] << 8) | vc_prov_pad[13]);
    if (padC > 0) {
        uint8_t *skipbuf = (uint8_t *)malloc(padC);
        if (!skipbuf) return -1;
        int rc = recv_all_opt(fd, out_recv, skipbuf, padC, stop);
        free(skipbuf);
        if (rc != 0) return -1;
    }

    uint8_t ia_len_b[2];
    if (recv_all_opt(fd, out_recv, ia_len_b, 2, stop) != 0) return -1;
    uint16_t ia_len = (uint16_t)(((uint16_t)ia_len_b[0] << 8) | ia_len_b[1]);
    if (ia_len != 68) return -1;
    if (recv_all_opt(fd, out_recv, peer_hs, 68, stop) != 0) return -1;

    // Reply: VC(8 zeros) + crypto_select(0x00000002 = RC4) + padD_len(0).
    uint8_t sel_reply[14];
    memset(sel_reply, 0, sizeof(sel_reply));
    sel_reply[11] = 0x02;
    if (send_all_opt(fd, out_send, sel_reply, sizeof(sel_reply), stop) != 0) return -1;
    return 0;
}

typedef struct {
    int            listen_fd;
    const uint8_t *info_hash;
    const uint8_t *meta;
    size_t         meta_len;
    bool           send_big_bitfield;
    bool           mse_only;
    int            reject_mode;      // 0=serve all, 1=reject odd pieces, 2=reject all pieces
    uint32_t       pex_ip_net;
    uint16_t       pex_port_host;
    volatile bool  stop;
    volatile int   plain_drops;
    volatile int   rejects_sent;
    volatile int   pieces_served;
} mock_peer_cfg;

static void serve_connected_peer(int cli, mock_peer_cfg *cfg, bool recv_hs, bool send_hs,
                                 bool encrypted, mse_rc4 *s_rc4, mse_rc4 *r_rc4) {
    mse_rc4 *enc = encrypted ? s_rc4 : NULL;
    mse_rc4 *dec = encrypted ? r_rc4 : NULL;

    uint8_t hs[68];
    if (recv_hs) {
        if (recv_all_opt(cli, dec, hs, 68, &cfg->stop) != 0) { close(cli); return; }
        if (hs[0] != 19 || memcmp(hs + 1, "BitTorrent protocol", 19) != 0 ||
            memcmp(hs + 28, cfg->info_hash, 20) != 0) {
            close(cli);
            return;
        }
    }

    if (send_hs) {
        uint8_t hs_rep[68];
        build_bt_hs(hs_rep, cfg->info_hash, "-TSMOCK-000000000001");
        if (send_all_opt(cli, enc, hs_rep, 68, &cfg->stop) != 0) { close(cli); return; }
    }

    if (cfg->send_big_bitfield) {
        // Send a 65536-byte BITFIELD (exceeds 64 KB buffer with 5-byte header) + 30 HAVE frames.
        size_t bf_len = 65536;
        uint8_t *bf = (uint8_t *)malloc(bf_len);
        if (!bf) { close(cli); return; }
        memset(bf, 0xFF, bf_len);
        int rc = send_bt_frame(cli, enc, 5, bf, bf_len, &cfg->stop);
        free(bf);
        if (rc != 0) { close(cli); return; }

        for (uint32_t i = 0; i < 30; i++) {
            uint32_t idx_be = htonl(i);
            if (send_bt_frame(cli, enc, 4, &idx_be, 4, &cfg->stop) != 0) { close(cli); return; }
        }
    }

    // Send BEP-10 ext handshake advertising ut_metadata=7, ut_pex=1.
    {
        char body[128];
        int blen = snprintf(body, sizeof(body),
                            "d1:md11:ut_metadatai7e6:ut_pexi1ee13:metadata_sizei%zuee",
                            cfg->meta_len);
        uint8_t *pkt = (uint8_t *)malloc((size_t)(blen + 1));
        pkt[0] = 0; // ext_id = 0 (handshake)
        memcpy(pkt + 1, body, (size_t)blen);
        int rc = send_bt_frame(cli, enc, 20, pkt, (size_t)(blen + 1), &cfg->stop);
        free(pkt);
        if (rc != 0) { close(cli); return; }
    }

    int peer_meta_id = EXT_ID_METADATA;
    int peer_pex_id = EXT_ID_PEX;
    bool pex_sent = false;

    while (!cfg->stop) {
        uint8_t len4[4];
        if (recv_all_opt(cli, dec, len4, 4, &cfg->stop) != 0) break;
        uint32_t flen = ((uint32_t)len4[0] << 24) | ((uint32_t)len4[1] << 16) |
                        ((uint32_t)len4[2] << 8) | (uint32_t)len4[3];
        if (flen == 0) continue;
        if (flen > 131072) break;
        uint8_t *msg = (uint8_t *)malloc(flen);
        if (!msg) break;
        if (recv_all_opt(cli, dec, msg, flen, &cfg->stop) != 0) {
            free(msg);
            break;
        }
        if (msg[0] == 20 && flen >= 2) {
            uint8_t eid = msg[1];
            be_node *doc = be_parse((const char *)(msg + 2), flen - 2);
            if (eid == 0 && doc && doc->type == BE_DICT) {
                be_node *md = be_dict_get(doc, "m");
                if (md && md->type == BE_DICT) {
                    be_node *um = be_dict_get(md, "ut_metadata");
                    be_node *up = be_dict_get(md, "ut_pex");
                    if (um && um->type == BE_INT && um->i > 0) peer_meta_id = (int)um->i;
                    if (up && up->type == BE_INT && up->i > 0) peer_pex_id = (int)up->i;
                }
                if (!pex_sent && cfg->pex_ip_net != 0 && cfg->pex_port_host != 0) {
                    pex_sent = true;
                    uint8_t pex_pkt[1 + 64];
                    int pos = 0;
                    pex_pkt[pos++] = (uint8_t)peer_pex_id;
                    const char *p1 = "d5:added6:";
                    memcpy(pex_pkt + pos, p1, strlen(p1)); pos += (int)strlen(p1);
                    memcpy(pex_pkt + pos, &cfg->pex_ip_net, 4); pos += 4;
                    uint16_t p_be = htons(cfg->pex_port_host);
                    memcpy(pex_pkt + pos, &p_be, 2); pos += 2;
                    const char *p2 = "7:added.f1:\x00" "e";
                    memcpy(pex_pkt + pos, p2, 13); pos += 13;
                    send_bt_frame(cli, enc, 20, pex_pkt, (size_t)pos, &cfg->stop);
                }
            } else if (eid == 7 && doc && doc->type == BE_DICT) {
                be_node *mt = be_dict_get(doc, "msg_type");
                be_node *pi = be_dict_get(doc, "piece");
                if (mt && mt->type == BE_INT && mt->i == 0 &&
                    pi && pi->type == BE_INT && pi->i >= 0) {
                    size_t idx = (size_t)pi->i;
                    size_t off = idx * 16384;
                    bool rej = (off >= cfg->meta_len) ||
                               (cfg->reject_mode == 2) ||
                               (cfg->reject_mode == 1 && (idx % 2 == 1));
                    if (rej) {
                        char rbody[96];
                        int rlen = snprintf(rbody, sizeof(rbody),
                                            "d8:msg_typei2e5:piecei%zuee", idx);
                        uint8_t *rpkt = (uint8_t *)malloc((size_t)(1 + rlen));
                        rpkt[0] = (uint8_t)peer_meta_id;
                        memcpy(rpkt + 1, rbody, (size_t)rlen);
                        send_bt_frame(cli, enc, 20, rpkt, (size_t)(1 + rlen), &cfg->stop);
                        free(rpkt);
                        cfg->rejects_sent++;
                    } else {
                        size_t rem = cfg->meta_len - off;
                        size_t chunk = rem > 16384 ? 16384 : rem;
                        char dbody[128];
                        int blen2 = snprintf(dbody, sizeof(dbody),
                                             "d8:msg_typei1e5:piecei%zue10:total_sizei%zuee",
                                             idx, cfg->meta_len);
                        uint8_t *dpkt = (uint8_t *)malloc((size_t)(1 + blen2) + chunk);
                        dpkt[0] = (uint8_t)peer_meta_id;
                        memcpy(dpkt + 1, dbody, (size_t)blen2);
                        memcpy(dpkt + 1 + blen2, cfg->meta + off, chunk);
                        send_bt_frame(cli, enc, 20, dpkt, (size_t)(1 + blen2) + chunk, &cfg->stop);
                        free(dpkt);
                        cfg->pieces_served++;
                    }
                }
            }
            if (doc) be_free(doc);
        }
        free(msg);
    }

    close(cli);
}

static void mock_peer_listener_fn(void *arg) {
    mock_peer_cfg *cfg = (mock_peer_cfg *)arg;
    while (!cfg->stop) {
        struct pollfd pfd = { (SOCKET)cfg->listen_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 30);
        if (pr <= 0 || !(pfd.revents & POLLIN)) continue;
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int cli = accept(cfg->listen_fd, (struct sockaddr *)&sa, &sl);
        if (cli < 0) continue;

        serve_connected_peer(cli, cfg, true, true, false, NULL, NULL);
    }
}

static void mock_mse_listener_fn(void *arg) {
    mock_peer_cfg *cfg = (mock_peer_cfg *)arg;
    while (!cfg->stop) {
        struct pollfd pfd = { (SOCKET)cfg->listen_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 30);
        if (pr <= 0 || !(pfd.revents & POLLIN)) continue;
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int cli = accept(cfg->listen_fd, (struct sockaddr *)&sa, &sl);
        if (cli < 0) continue;

        uint8_t peek = 0;
        // Wait for first byte using MSG_PEEK so handshake helpers see all bytes cleanly.
        while (!cfg->stop) {
            struct pollfd rp = { (SOCKET)cli, POLLIN, 0 };
            int r = poll(&rp, 1, 30);
            if (r > 0) {
                ssize_t n = recv(cli, (char *)&peek, 1, MSG_PEEK);
                if (n <= 0) peek = 0;
                break;
            }
        }
        if (cfg->stop || peek == 0) { close(cli); continue; }
        if (peek == 19) {
            uint8_t hs[68];
            recv_all_opt(cli, NULL, hs, 68, &cfg->stop);
            cfg->plain_drops++;
            close(cli);
            continue;
        }

        mse_rc4 s_rc4, r_rc4;
        uint8_t peer_hs[68];
        if (mse_responder_handshake(cli, cfg->info_hash, &s_rc4, &r_rc4, peer_hs, &cfg->stop) != 0) {
            close(cli);
            continue;
        }
        serve_connected_peer(cli, cfg, false, true, true, &s_rc4, &r_rc4);
    }
}

TEST(bitfield_and_have_skip) {
    uint8_t ih[20];
    size_t mlen = 0;
    uint8_t *meta = build_test_info(200, ih, &mlen);
    ASSERT(meta != NULL);

    uint16_t port = 0;
    int lfd = bind_loopback_listener(&port);
    ASSERT(lfd >= 0);

    mock_peer_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.listen_fd = lfd;
    cfg.info_hash = ih;
    cfg.meta = meta;
    cfg.meta_len = mlen;
    cfg.send_big_bitfield = true;

    Thread th;
    ASSERT(threadCreate(&th, mock_peer_listener_fn, &cfg, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&th);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testbitfld01", 20);
    peer_addr pa = { htonl(INADDR_LOOPBACK), port };
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = &pa;
    opts.seed_count = 1;
    opts.budget_ms = 8000;

    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));

    cfg.stop = true;
    close(lfd);
    threadWaitForExit(&th);
    threadClose(&th);

    ASSERT_MSG(rc == 0, err);
    ASSERT(out != NULL && out_len == mlen);
    ASSERT(memcmp(out, meta, mlen) == 0);
    free(out);
    free(meta);
}

TEST(mse_only_fallback) {
    uint8_t ih[20];
    size_t mlen = 0;
    uint8_t *meta = build_test_info(200, ih, &mlen);
    ASSERT(meta != NULL);

    uint16_t port = 0;
    int lfd = bind_loopback_listener(&port);
    ASSERT(lfd >= 0);

    mock_peer_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.listen_fd = lfd;
    cfg.info_hash = ih;
    cfg.meta = meta;
    cfg.meta_len = mlen;
    cfg.mse_only = true;

    Thread th;
    ASSERT(threadCreate(&th, mock_mse_listener_fn, &cfg, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&th);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testmse00001", 20);
    peer_addr pa = { htonl(INADDR_LOOPBACK), port };
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = &pa;
    opts.seed_count = 1;
    opts.budget_ms = 8000;

    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));

    cfg.stop = true;
    close(lfd);
    threadWaitForExit(&th);
    threadClose(&th);

    ASSERT(cfg.plain_drops >= 1);
    ASSERT(rc == 0);
    ASSERT(out != NULL && out_len == mlen);
    ASSERT(memcmp(out, meta, mlen) == 0);
    free(out);
    free(meta);
}

TEST(multi_peer_reject_and_assembly) {
    // 6100 * 20 = 122000 bytes -> ~122 KB -> 8 metadata pieces (16384 B each).
    uint8_t ih[20];
    size_t mlen = 0;
    uint8_t *meta = build_test_info(6100, ih, &mlen);
    ASSERT(meta != NULL);
    ASSERT(mlen > 7 * 16384 && mlen <= 8 * 16384);

    uint16_t portA = 0, portB = 0;
    int lfdA = bind_loopback_listener(&portA);
    int lfdB = bind_loopback_listener(&portB);
    ASSERT(lfdA >= 0 && lfdB >= 0);

    mock_peer_cfg cfgA, cfgB;
    memset(&cfgA, 0, sizeof(cfgA));
    memset(&cfgB, 0, sizeof(cfgB));
    cfgA.listen_fd = lfdA; cfgA.info_hash = ih; cfgA.meta = meta; cfgA.meta_len = mlen; cfgA.reject_mode = 1; // reject odd
    cfgB.listen_fd = lfdB; cfgB.info_hash = ih; cfgB.meta = meta; cfgB.meta_len = mlen; cfgB.reject_mode = 0; // serve all

    Thread thA, thB;
    ASSERT(threadCreate(&thA, mock_peer_listener_fn, &cfgA, NULL, 0x8000, 0x2C, -2) == 0);
    ASSERT(threadCreate(&thB, mock_peer_listener_fn, &cfgB, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&thA);
    threadStart(&thB);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testmulti001", 20);
    peer_addr pas[2] = {
        { htonl(INADDR_LOOPBACK), portA },
        { htonl(INADDR_LOOPBACK), portB }
    };
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = pas;
    opts.seed_count = 2;
    opts.budget_ms = 10000;

    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));

    cfgA.stop = true;
    cfgB.stop = true;
    close(lfdA);
    close(lfdB);
    threadWaitForExit(&thA); threadClose(&thA);
    threadWaitForExit(&thB); threadClose(&thB);

    ASSERT(rc == 0);
    ASSERT(out != NULL && out_len == mlen);
    ASSERT(memcmp(out, meta, mlen) == 0);
    ASSERT(cfgB.pieces_served >= 4);
    free(out);
    free(meta);
}

TEST(ut_pex_discovery) {
    uint8_t ih[20];
    size_t mlen = 0;
    uint8_t *meta = build_test_info(200, ih, &mlen);
    ASSERT(meta != NULL);

    uint16_t portA = 0, portB = 0;
    int lfdA = bind_loopback_listener(&portA);
    int lfdB = bind_loopback_listener(&portB);
    ASSERT(lfdA >= 0 && lfdB >= 0);

    mock_peer_cfg cfgA, cfgB;
    memset(&cfgA, 0, sizeof(cfgA));
    memset(&cfgB, 0, sizeof(cfgB));
    // Peer A rejects all metadata, but sends Peer B via ut_pex.
    cfgA.listen_fd = lfdA; cfgA.info_hash = ih; cfgA.meta = meta; cfgA.meta_len = mlen;
    cfgA.reject_mode = 2; cfgA.pex_ip_net = htonl(INADDR_LOOPBACK); cfgA.pex_port_host = portB;
    cfgB.listen_fd = lfdB; cfgB.info_hash = ih; cfgB.meta = meta; cfgB.meta_len = mlen;
    cfgB.reject_mode = 0;

    Thread thA, thB;
    ASSERT(threadCreate(&thA, mock_peer_listener_fn, &cfgA, NULL, 0x8000, 0x2C, -2) == 0);
    ASSERT(threadCreate(&thB, mock_peer_listener_fn, &cfgB, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&thA);
    threadStart(&thB);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testpex00001", 20);
    peer_addr paA = { htonl(INADDR_LOOPBACK), portA };
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = &paA;
    opts.seed_count = 1;
    opts.budget_ms = 10000;

    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));

    cfgA.stop = true;
    cfgB.stop = true;
    close(lfdA);
    close(lfdB);
    threadWaitForExit(&thA); threadClose(&thA);
    threadWaitForExit(&thB); threadClose(&thB);

    ASSERT(rc == 0);
    ASSERT(out != NULL && out_len == mlen);
    ASSERT(memcmp(out, meta, mlen) == 0);
    ASSERT(cfgB.pieces_served > 0);
    free(out);
    free(meta);
}

typedef struct {
    const uint8_t *ih;
    mock_peer_cfg *cfg;
    volatile bool routed;
} incoming_inject_ctx;

static void incoming_inject_fn(void *arg) {
    incoming_inject_ctx *ic = (incoming_inject_ctx *)arg;
    uint16_t lport = 0;
    int lfd = bind_loopback_listener(&lport);
    if (lfd < 0) return;

    int peer_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_fd < 0) { close(lfd); return; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(lport);
    if (connect(peer_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(peer_fd); close(lfd); return;
    }

    struct sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    int eng_fd = accept(lfd, (struct sockaddr *)&ca, &cl);
    close(lfd);
    if (eng_fd < 0) { close(peer_fd); return; }

    // Peer sends BT handshake (with BEP-10 ext bit) to eng_fd.
    uint8_t peer_hs[68];
    build_bt_hs(peer_hs, ic->ih, "-TSMOCK-INCOMING0001");
    if (send_all_opt(peer_fd, NULL, peer_hs, 68, &ic->cfg->stop) != 0) {
        close(peer_fd); close(eng_fd); return;
    }

    uint8_t got_hs[68];
    if (recv_all_opt(eng_fd, NULL, got_hs, 68, &ic->cfg->stop) != 0) {
        close(peer_fd); close(eng_fd); return;
    }

    // Wait briefly until meta_fetch_run() registers its context, then hand eng_fd to it.
    bool ok = false;
    for (int i = 0; i < 60 && !ic->cfg->stop; i++) {
        if (meta_fetch_route_incoming(ic->ih, eng_fd, got_hs)) {
            ok = true;
            break;
        }
        svcSleepThread(10 * 1000 * 1000ULL);
    }
    ic->routed = ok;
    if (!ok) {
        close(eng_fd);
        close(peer_fd);
        return;
    }

    // meta_fetch_route_incoming now sends its 68-byte handshake reply back + BEP-10 + BEP-9.
    serve_connected_peer(peer_fd, ic->cfg, true, false, false, NULL, NULL);
}

TEST(incoming_routing) {
    uint8_t ih[20];
    size_t mlen = 0;
    uint8_t *meta = build_test_info(200, ih, &mlen);
    ASSERT(meta != NULL);

    mock_peer_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.listen_fd = -1;
    cfg.info_hash = ih;
    cfg.meta = meta;
    cfg.meta_len = mlen;

    incoming_inject_ctx ic = { ih, &cfg, false };
    Thread th;
    ASSERT(threadCreate(&th, incoming_inject_fn, &ic, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&th);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testincom001", 20);
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = NULL;
    opts.seed_count = 0;
    opts.use_dht = false;
    opts.budget_ms = 5000;

    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));

    cfg.stop = true;
    threadWaitForExit(&th);
    threadClose(&th);

    ASSERT(ic.routed);
    ASSERT(rc == 0);
    ASSERT(out != NULL && out_len == mlen);
    ASSERT(memcmp(out, meta, mlen) == 0);
    free(out);
    free(meta);
}

static void cancel_after_delay_fn(void *arg) {
    volatile bool *c = (volatile bool *)arg;
    svcSleepThread(50 * 1000 * 1000ULL);
    *c = true;
}

TEST(fast_cancel) {
    uint8_t ih[20];
    for (int i = 0; i < 20; i++) ih[i] = (uint8_t)(0x55 + i);

    uint16_t port = 0;
    int lfd = bind_loopback_listener(&port);
    ASSERT(lfd >= 0);

    volatile bool cancel = false;
    Thread th;
    ASSERT(threadCreate(&th, cancel_after_delay_fn, (void *)&cancel, NULL, 0x4000, 0x2C, -2) == 0);
    threadStart(&th);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testcncl0001", 20);
    peer_addr pa = { htonl(INADDR_LOOPBACK), port };
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = &pa;
    opts.seed_count = 1;
    opts.budget_ms = 30000;
    opts.cancel = &cancel;

    uint64_t t0 = now_ms();
    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));
    uint64_t dt = now_ms() - t0;

    threadWaitForExit(&th);
    threadClose(&th);
    close(lfd);

    ASSERT(rc != 0);
    ASSERT(out == NULL);
    ASSERT(dt < 250);
}

typedef struct {
    int           fds[40];
    int           nfds;
    volatile bool stop;
} silent_multi_cfg;

static void silent_multi_fn(void *arg) {
    silent_multi_cfg *cfg = (silent_multi_cfg *)arg;
    int clis[128];
    int ncli = 0;
    struct pollfd pfds[40];
    while (!cfg->stop) {
        for (int i = 0; i < cfg->nfds; i++) {
            pfds[i].fd = (SOCKET)cfg->fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        int pr = poll(pfds, (nfds_t)cfg->nfds, 30);
        if (pr <= 0) continue;
        for (int i = 0; i < cfg->nfds; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct sockaddr_in sa;
            socklen_t sl = sizeof(sa);
            int c = accept(cfg->fds[i], (struct sockaddr *)&sa, &sl);
            if (c >= 0) {
                if (ncli < 128) clis[ncli++] = c;
                else close(c);
            }
        }
    }
    for (int i = 0; i < ncli; i++) close(clis[i]);
}

TEST(silent_peer_handshake_timeout) {
    uint8_t ih[20];
    for (int i = 0; i < 20; i++) ih[i] = (uint8_t)(0x77 + i);

    uint16_t port = 0;
    int lfd = bind_loopback_listener(&port);
    ASSERT(lfd >= 0);

    silent_multi_cfg scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.fds[0] = lfd;
    scfg.nfds = 1;

    Thread th;
    ASSERT(threadCreate(&th, silent_multi_fn, &scfg, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&th);

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testsilent01", 20);
    peer_addr pa = { htonl(INADDR_LOOPBACK), port };

    uint64_t t0 = now_ms();
    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_single_peer(pa, ih, pid, NULL, &out, &out_len, err, sizeof(err));
    uint64_t dt = now_ms() - t0;

    scfg.stop = true;
    close(lfd);
    threadWaitForExit(&th);
    threadClose(&th);

    ASSERT(rc != 0);
    ASSERT(out == NULL);
    ASSERT_MSG(strstr(err, "handshake timeout") != NULL, err);
    /* Plain pre-hs timeout (3.5s) + single MSE retry (3.5s) exits well before 10s idle / 12s budget. */
    ASSERT(dt >= 6000 && dt < 9000);
}

TEST(active_close_mse_priority_over_untried) {
    uint8_t ih[20];
    size_t mlen = 0;
    uint8_t *meta = build_test_info(200, ih, &mlen);
    ASSERT(meta != NULL);

    silent_multi_cfg scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.nfds = 32;
    uint16_t sports[32];
    for (int i = 0; i < 32; i++) {
        scfg.fds[i] = bind_loopback_listener(&sports[i]);
        ASSERT(scfg.fds[i] >= 0);
    }

    uint16_t mse_port = 0;
    int mse_lfd = bind_loopback_listener(&mse_port);
    ASSERT(mse_lfd >= 0);

    mock_peer_cfg mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.listen_fd = mse_lfd;
    mcfg.info_hash = ih;
    mcfg.meta = meta;
    mcfg.meta_len = mlen;
    mcfg.mse_only = true;

    Thread sth, mth;
    ASSERT(threadCreate(&sth, silent_multi_fn, &scfg, NULL, 0x8000, 0x2C, -2) == 0);
    ASSERT(threadCreate(&mth, mock_mse_listener_fn, &mcfg, NULL, 0x8000, 0x2C, -2) == 0);
    threadStart(&sth);
    threadStart(&mth);

    peer_addr pas[33];
    for (int i = 0; i < 12; i++) {
        pas[i].ip = htonl(INADDR_LOOPBACK);
        pas[i].port = sports[i];
    }
    pas[12].ip = htonl(INADDR_LOOPBACK);
    pas[12].port = mse_port;
    for (int i = 0; i < 20; i++) {
        pas[13 + i].ip = htonl(INADDR_LOOPBACK);
        pas[13 + i].port = sports[12 + i];
    }

    uint8_t pid[20];
    memcpy(pid, "-TS9999-testmseprio1", 20);
    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = pas;
    opts.seed_count = 33;
    opts.budget_ms = 10000;

    uint64_t t0 = now_ms();
    uint8_t *out = NULL;
    size_t out_len = 0;
    char err[128] = {0};
    int rc = meta_fetch_run(&opts, &out, &out_len, NULL, 0, NULL, err, sizeof(err));
    uint64_t dt = now_ms() - t0;

    scfg.stop = true;
    mcfg.stop = true;
    for (int i = 0; i < 32; i++) close(scfg.fds[i]);
    close(mse_lfd);
    threadWaitForExit(&sth);
    threadWaitForExit(&mth);
    threadClose(&sth);
    threadClose(&mth);

    ASSERT_MSG(rc == 0, err);
    ASSERT(out != NULL && out_len == mlen);
    ASSERT(memcmp(out, meta, mlen) == 0);
    ASSERT(mcfg.plain_drops >= 1);
    ASSERT(dt < 3000);
    free(out);
    free(meta);
}

TEST(preserve_peers_on_failure_and_filter_self) {
    int saved_port = torrent_announce_port();
    torrent_clear_external_ip();
    torrent_set_announce_port(6882);

    uint32_t pub = inet_addr("95.27.161.169");
    torrent_set_external_ip(pub);

    uint16_t dead_port = 0;
    int dead_fd = bind_loopback_listener(&dead_port);
    ASSERT(dead_fd >= 0);
    close(dead_fd);

    uint8_t ih[20], pid[20];
    memset(ih, 0x42, sizeof(ih));
    memcpy(pid, "-TS9999-testpreserve", 20);

    peer_addr seeds[2];
    seeds[0].ip = pub;
    seeds[0].port = 6882;
    seeds[1].ip = htonl(INADDR_LOOPBACK);
    seeds[1].port = dead_port;

    meta_fetch_opts opts = {0};
    opts.info_hash = ih;
    opts.peer_id = pid;
    opts.seed_peers = seeds;
    opts.seed_count = 2;
    opts.budget_ms = 2000;

    uint8_t *out = NULL;
    size_t out_len = 0;
    peer_addr out_peers[8];
    int out_n = -1;
    char err[128] = {0};

    int rc = meta_fetch_run(&opts, &out, &out_len, out_peers, 8, &out_n, err, sizeof(err));
    ASSERT(rc != 0);
    ASSERT(out == NULL);
    ASSERT(out_n == 1);
    ASSERT(out_peers[0].ip == seeds[1].ip && out_peers[0].port == seeds[1].port);

    volatile bool cancel = true;
    opts.cancel = &cancel;
    out_n = -1;
    rc = meta_fetch_run(&opts, &out, &out_len, out_peers, 8, &out_n, err, sizeof(err));
    ASSERT(rc != 0);
    ASSERT(out_n == 0);

    torrent_clear_external_ip();
    torrent_set_announce_port(saved_port);
}

int main(void) {
    meta_fetch_init_early();
    printf("Running meta_fetch unit tests:\n");
    RUN(bitfield_and_have_skip);
    RUN(mse_only_fallback);
    RUN(multi_peer_reject_and_assembly);
    RUN(ut_pex_discovery);
    RUN(incoming_routing);
    RUN(fast_cancel);
    RUN(silent_peer_handshake_timeout);
    RUN(active_close_mse_priority_over_untried);
    RUN(preserve_peers_on_failure_and_filter_self);
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
