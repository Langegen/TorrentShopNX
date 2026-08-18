#ifndef MSE_PEER_H
#define MSE_PEER_H

// Message Stream Encryption (MSE/PE), the outgoing (initiator) side only.
// We never accept incoming connections, so the responder side is not needed.
//
// Protocol sketch (per the Vuze MSE/PE description, as implemented by
// libtorrent/qBittorrent):
//   initiator -> pubA + padA (padA = 0 here)
//   responder -> pubB + padB (pubB's first byte is NOT restricted)
//   S = pubB^privA mod P;  keyA = SHA1("keyA"|S|info_hash);
//   keyB = SHA1("keyB"|S|info_hash);  both RC4 streams discard 1024 bytes
//   initiator -> req1(20) | req2 XOR req3(20) | rc4(keyA): VC(8 zeros)
//                 + crypto_provide(4 BE: RC4|plaintext) + padC_len(2 BE)
//                 + ia_len(2 BE) + BT handshake (the "initial payload")
//   responder -> rc4(keyB): VC(8 zeros) + crypto_select(4 BE) + padD_len(2 BE)
//                 + padD
//   afterwards the whole BT stream is RC4, keyA out / keyB in.
//   req1 = SHA1("req1"|S), req2 = SHA1("req2"|info_hash),
//   req3 = SHA1("req3"|S); the responder verifies req1 and req2 XOR req3,
//   and resyncs on the encrypted VC (keystream prefix of the key it uses to
//   read our stream) with a sliding search.
//
// The handshake is buffer-driven: the caller feeds received bytes and drains
// produced bytes, so this runs identically over TCP and uTP.
//
// DH uses the mbedtls bignum (768-bit prime), SHA-1 via mbedtls, RC4 is our
// own -- the protocol logic is the same on Switch and PC.

#include <stddef.h>
#include <stdint.h>

#define MSE_DH_LEN 96   // 768-bit DH public key / shared secret, big-endian

typedef struct {
    uint8_t s[256];
    uint8_t i, j;
} mse_rc4;

void mse_rc4_init(mse_rc4 *r, const uint8_t *key, size_t key_len);
// XOR `len` keystream bytes over in->out (in == out allowed).
void mse_rc4_crypt(mse_rc4 *r, const uint8_t *in, uint8_t *out, size_t len);
// Discard `n` keystream bytes (MSE requires discarding the first 1024).
void mse_rc4_skip(mse_rc4 *r, size_t n);

// Feed result codes.
enum {
    MSE_CONT  = 0,   // progress made; feed the remaining input again
    MSE_DONE  = 1,   // handshake complete; stream crypto is live
    MSE_FAIL  = -1,  // peer is not MSE-capable (bad framing/select)
    MSE_PLAIN = -2   // peer answered with a plaintext BT handshake: not MSE
};

typedef struct {
    int phase;              // 0 = await pubB, 1 = await VC, 2 = select/padD
    int started;            // mse_peer_start ran
    uint8_t priv[MSE_DH_LEN];
    uint8_t info_hash[20];
    uint8_t ia[68];         // BT handshake piggybacked as the initial payload
    size_t ia_len;

    mse_rc4 send_rc4;       // keyA: our outbound stream
    mse_rc4 recv_rc4;       // keyB: our inbound stream
    uint8_t vc[8];          // first 8 bytes of the recv keystream (encrypted VC)

    uint8_t selbuf[6];      // crypto_select(4) + padD_len(2), decrypted
    size_t sel_have;
    uint32_t pad_skip;      // padD bytes still to skip
    bool rc4_selected;      // crypto_select picked RC4 (vs plaintext)

    int dh_attempts;        // pubA regenerations, for diagnostics
} mse_peer;

// Begin the handshake: generate the DH key, emit pubA (padA = 0). `ia` is the
// initial payload (the 68-byte BT handshake) that rides inside the encrypted
// request. Writes the bytes to send into `out`, sets *out_len.
void mse_peer_start(mse_peer *m, const uint8_t info_hash[20],
                    const uint8_t *ia, size_t ia_len,
                    uint8_t *out, size_t *out_len);

// Feed received bytes. *consumed = how many input bytes were used, and bytes
// to send are written to `out` (*produced). Returns MSE_CONT (call again with
// the remaining input), MSE_DONE (call mse_peer_decrypt for the buffered
// remainder), MSE_FAIL or MSE_PLAIN.
int mse_peer_feed(mse_peer *m, const uint8_t *in, size_t in_len,
                  size_t *consumed, uint8_t *out, size_t out_cap,
                  size_t *produced);

// Decrypt `len` bytes of the peer's post-handshake stream in place.
// Only valid after MSE_DONE.
void mse_peer_decrypt(mse_peer *m, uint8_t *buf, size_t len);

#endif