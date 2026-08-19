#include "mse_peer.h"

#include <string.h>

#include <mbedtls/bignum.h>
#include <mbedtls/sha1.h>

#include "switch.h"  // randomGet (libnx on Switch, compat shim on PC)

// Canonical MSE/PE 768-bit prime (Vuze/libtorrent), big-endian, g = 2.
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

// ---- RC4 ----

void mse_rc4_init(mse_rc4 *r, const uint8_t *key, size_t key_len) {
    for (int n = 0; n < 256; ++n)
        r->s[n] = (uint8_t)n;
    r->i = r->j = 0;
    uint8_t j = 0;
    for (int n = 0; n < 256; ++n) {
        j = (uint8_t)(j + r->s[n] + key[n % key_len]);
        uint8_t t = r->s[n];
        r->s[n] = r->s[j];
        r->s[j] = t;
    }
}

void mse_rc4_crypt(mse_rc4 *r, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t i = r->i, j = r->j;
    for (size_t n = 0; n < len; ++n) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + r->s[i]);
        uint8_t t = r->s[i];
        r->s[i] = r->s[j];
        r->s[j] = t;
        uint8_t k = r->s[(uint8_t)(r->s[i] + r->s[j])];
        out[n] = (uint8_t)(in[n] ^ k);
    }
    r->i = i;
    r->j = j;
}

void mse_rc4_skip(mse_rc4 *r, size_t n) {
    uint8_t i = r->i, j = r->j;
    for (size_t k = 0; k < n; ++k) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + r->s[i]);
        uint8_t t = r->s[i];
        r->s[i] = r->s[j];
        r->s[j] = t;
    }
    r->i = i;
    r->j = j;
}

// ---- Diffie-Hellman (g = 2, P = MSE 768-bit prime) ----

// Compute base^exp mod P into out (MSE_DH_LEN big-endian).
static void dh_modexp(const uint8_t *base, size_t base_len,
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
    size_t olen = 0;
    mbedtls_mpi_write_binary(&r, out, MSE_DH_LEN);
    (void)olen;
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&r);
}

// Fresh 160-bit private exponent, zero-padded to MSE_DH_LEN.
static void dh_private(uint8_t priv[MSE_DH_LEN]) {
    memset(priv, 0, MSE_DH_LEN - 20);
    randomGet(priv + MSE_DH_LEN - 20, 20);
}

// pub = 2^priv mod P, regenerating priv until the first byte is 0x00..0x02
// (a plaintext BT handshake starts with 0x13, so this keeps the two frames
// distinguishable; libtorrent does the same).
static void dh_public(uint8_t pub[MSE_DH_LEN], uint8_t priv[MSE_DH_LEN],
                      int *attempts) {
    static const uint8_t two[1] = { 2 };
    for (;;) {
        dh_private(priv);
        dh_modexp(two, 1, priv, pub);
        (*attempts)++;
        if (pub[0] <= 2 && !(pub[0] == 0 && pub[1] == 0))
            break;
    }
}

// SHA1(tag | data) into out[20]; with d2 non-NULL: SHA1(tag | d1 | d2).
static void sha1_concat(const uint8_t *tag, size_t tag_len,
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

// Derive keyA/keyB: SHA1(label | S | SKEY), SKEY = info hash.
static void stream_key(const char label[4], const uint8_t secret[MSE_DH_LEN],
                       const uint8_t info_hash[20], mse_rc4 *out) {
    uint8_t h[20];
    sha1_concat((const uint8_t *)label, 4, secret, MSE_DH_LEN, info_hash, 20, h);
    mse_rc4_init(out, h, sizeof(h));
    mse_rc4_skip(out, 1024);
}

void mse_peer_start(mse_peer *m, const uint8_t info_hash[20],
                    const uint8_t *ia, size_t ia_len,
                    uint8_t *out, size_t *out_len) {
    memset(m, 0, sizeof(*m));
    m->started = 1;
    m->phase = 0;
    m->ia_len = ia_len;
    memcpy(m->info_hash, info_hash, 20);
    if (ia_len) memcpy(m->ia, ia, ia_len);

    dh_public(out, m->priv, &m->dh_attempts);
    *out_len = MSE_DH_LEN;  // pubA + padA (padA = 0)
}

int mse_peer_feed(mse_peer *m, const uint8_t *in, size_t in_len,
                  size_t *consumed, uint8_t *out, size_t out_cap,
                  size_t *produced) {
    *consumed = 0;
    *produced = 0;
    if (!m->started) return MSE_FAIL;

    if (m->phase == 0) {
        // Await pubB (+padB). A plaintext responder answers our pubA with a
        // normal 68-byte BT handshake: first byte 0x13. Any other 96-byte
        // reply is treated as the DH public key (responders do not restrict
        // pubB's first byte to 0x00..0x02 the way initiators must).
        if (in_len == 0) return MSE_CONT;
        if (in[0] == 19) return MSE_PLAIN;
        if (in_len < MSE_DH_LEN) return MSE_CONT;  // pubB not whole yet

        // Shared secret S = pubB^privA mod P.
        uint8_t secret[MSE_DH_LEN];
        dh_modexp(in, MSE_DH_LEN, m->priv, secret);

        // RC4 streams, each already 1024 bytes into its keystream.
        stream_key("keyA", secret, m->info_hash, &m->send_rc4);
        stream_key("keyB", secret, m->info_hash, &m->recv_rc4);

        // The encrypted VC is 8 zero bytes under keyB: its ciphertext is the
        // first 8 keystream bytes. We resync on this pattern in the stream
        // because padB may be up to 512 bytes (a copy of recv_rc4, so the
        // live stream state is untouched).
        mse_rc4 probe = m->recv_rc4;
        uint8_t zeros[8] = {0};
        mse_rc4_crypt(&probe, zeros, m->vc, 8);

        // Request: req1 (20) | req2 XOR req3 (20), then RC4(keyA) over
        // VC(8 zeros) | crypto_provide(4 BE: RC4|plaintext) | padC_len(2 BE,
        // 0) | ia_len(2 BE) | IA. Fields are network byte order, as the
        // canonical clients (libtorrent) read them.
        uint8_t h1[20], h2[20], h3[20];
        sha1_concat((const uint8_t *)"req1", 4, secret, MSE_DH_LEN, NULL, 0, h1);
        sha1_concat((const uint8_t *)"req2", 4, m->info_hash, 20, NULL, 0, h2);
        sha1_concat((const uint8_t *)"req3", 4, secret, MSE_DH_LEN, NULL, 0, h3);
        uint8_t block[8 + 4 + 2 + 2 + 68];
        size_t blen = 0;
        memset(block + blen, 0, 8); blen += 8;        // VC
        block[blen + 0] = 0x00;                       // crypto_provide, BE
        block[blen + 1] = 0x00;
        block[blen + 2] = 0x00;
        block[blen + 3] = 0x03;                       // RC4 | plaintext
        blen += 4;
        block[blen + 0] = 0x00;                       // padC_len = 0, BE
        block[blen + 1] = 0x00;
        blen += 2;
        block[blen + 0] = (uint8_t)(m->ia_len >> 8);  // ia_len (68), BE
        block[blen + 1] = (uint8_t)m->ia_len;
        blen += 2;
        memcpy(block + blen, m->ia, m->ia_len);
        blen += m->ia_len;
        if (20 + 20 + blen > out_cap) return MSE_FAIL;
        memcpy(out, h1, 20);
        for (int i = 0; i < 20; i++) out[20 + i] = (uint8_t)(h2[i] ^ h3[i]);
        mse_rc4_crypt(&m->send_rc4, block, out + 40, blen);
        *produced = 40 + blen;

        *consumed = MSE_DH_LEN;  // padB and later bytes stay buffered
        m->phase = 1;
        return MSE_CONT;
    }

    if (m->phase == 1) {
        // Resync on the encrypted VC: the VC is 8 zero bytes under keyB, so
        // its ciphertext is a fixed 8-byte keystream prefix. A responder with
        // padB == 0 puts it at offset 0; with padB > 0 the offset shifts by
        // padB_len, so we scan every plausible offset and verify each by
        // decrypting the candidate with a COPY of the RC4 state (the live
        // stream only advances on a confirmed match). The scan must keep the
        // trailing 7 bytes buffered: a VC split across two feeds would
        // otherwise be missed.
        size_t pos = 0;
        size_t limit = in_len >= 8 ? in_len - 8 : 0;
        if (limit > 512) limit = 512;   // padB is at most 512 bytes
        uint8_t zeros[8] = {0};
        while (pos <= limit) {
            mse_rc4 probe = m->recv_rc4;
            mse_rc4_skip(&probe, pos);
            uint8_t expect[8];
            mse_rc4_crypt(&probe, zeros, expect, 8);
            if (memcmp(in + pos, expect, 8) == 0) {
                mse_rc4_skip(&m->recv_rc4, pos + 8);  // padB + VC
                m->sel_have = 0;
                m->pad_skip = 0;
                m->phase = 2;
                *consumed = pos + 8;
                return MSE_CONT;
            }
            pos++;
        }
        *consumed = in_len > 7 ? in_len - 7 : 0;
        return MSE_CONT;
    }

    // phase == 2: crypto_select(4) + padD_len(2), then padD, then the stream.
    size_t pos = 0;
    while (m->sel_have < 6 && pos < in_len) {
        uint8_t b = in[pos];
        mse_rc4_crypt(&m->recv_rc4, &b, &m->selbuf[m->sel_have], 1);
        m->sel_have++;
        pos++;
    }
    if (m->sel_have == 6 && m->pad_skip == 0 && m->phase == 2) {
        uint32_t sel = ((uint32_t)m->selbuf[0] << 24) |
                       ((uint32_t)m->selbuf[1] << 16) |
                       ((uint32_t)m->selbuf[2] << 8) |
                       ((uint32_t)m->selbuf[3]);
        uint32_t pad_len = ((uint32_t)m->selbuf[4] << 8) | m->selbuf[5];
        if (sel == 0) return MSE_FAIL;          // nothing acceptable
        if ((sel & 0x02u) != 0) {
            m->rc4_selected = true;             // RC4 chosen
        } else if ((sel & 0x01u) != 0) {
            m->rc4_selected = false;            // plaintext fallback
        } else {
            return MSE_FAIL;
        }
        m->pad_skip = pad_len;
    }
    while (m->pad_skip > 0 && pos < in_len) {
        size_t n = m->pad_skip < in_len - pos ? m->pad_skip : in_len - pos;
        mse_rc4_skip(&m->recv_rc4, n);
        m->pad_skip -= (uint32_t)n;
        pos += n;
    }
    *consumed = pos;
    if (m->sel_have < 6) return MSE_CONT;   // select+padD_len still on the wire
    if (m->pad_skip > 0) return MSE_CONT;   // padD still on the wire
    m->phase = 3;
    return MSE_DONE;
}

void mse_peer_decrypt(mse_peer *m, uint8_t *buf, size_t len) {
    mse_rc4_crypt(&m->recv_rc4, buf, buf, len);
}