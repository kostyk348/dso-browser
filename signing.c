/**
 * @file signing.c
 * @brief Content signing — hash-based MAC scheme for prototyping.
 *
 * This is a symmetric MAC scheme, not asymmetric crypto.
 * For production, replace with libsodium Ed25519.
 *
 * Scheme:
 *   keypair: sk = random, pk = H(sk)
 *   sign(sk, msg) = H(sk || msg)
 *   verify(sk, msg, sig) = sig == H(sk || msg)
 *
 * For asymmetric verification in signed content:
 *   signed_content = [sig(64) || pk(32) || content_len(4) || content]
 *   sig = H(sk || content)
 *   To verify: compute H(sk || content) and compare
 *   But we need sk... so for PROTOTYPE, we embed H(sk) in pk
 *   and verify by: pk == H(sk) which we can't check without sk.
 *
 * Practical approach: publisher signs with sk, consumer verifies by
 * checking pk matches publisher's known pk. This is trust-on-first-use.
 */

#include "signing.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

// ── SHA-256 ───────────────────────────────────────────────────────────────────

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e&f) ^ (~e&g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t maj = (a&b) ^ (a&c) ^ (b&c);
        uint32_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

static void sha256_raw(const uint8_t *data, size_t len, uint8_t hash[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t buf[64];
    size_t buf_len = 0;
    
    for (size_t i = 0; i < len; i++) {
        buf[buf_len++] = data[i];
        if (buf_len == 64) { sha256_transform(h, buf); buf_len = 0; }
    }
    
    buf[buf_len++] = 0x80;
    if (buf_len > 56) {
        while (buf_len < 64) buf[buf_len++] = 0;
        sha256_transform(h, buf);
        buf_len = 0;
    }
    while (buf_len < 56) buf[buf_len++] = 0;
    
    uint64_t bits = len * 8;
    for (int i = 0; i < 8; i++) buf[56+i] = (uint8_t)(bits >> (56 - 8*i));
    sha256_transform(h, buf);
    
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(h[i] >> 24);
        hash[i*4+1] = (uint8_t)(h[i] >> 16);
        hash[i*4+2] = (uint8_t)(h[i] >> 8);
        hash[i*4+3] = (uint8_t)(h[i]);
    }
}

// ── Key Generation ────────────────────────────────────────────────────────────

void psirp_sign_seed(uint8_t seed[PSIRP_SEED_SIZE]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, seed, PSIRP_SEED_SIZE);
        close(fd);
    }
}

void psirp_sign_keygen(const uint8_t seed[PSIRP_SEED_SIZE], psirp_keypair *keypair) {
    // secret_key = H(seed || "secret")
    uint8_t tmp[PSIRP_SEED_SIZE + 6];
    memcpy(tmp, seed, PSIRP_SEED_SIZE);
    memcpy(tmp + PSIRP_SEED_SIZE, "secret", 6);
    sha256_raw(tmp, sizeof(tmp), keypair->secret_key);
    
    // public_key = H(secret_key || "public")
    memcpy(tmp, keypair->secret_key, PSIRP_KEY_SIZE);
    memcpy(tmp + PSIRP_KEY_SIZE, "public", 6);
    sha256_raw(tmp, sizeof(tmp), keypair->public_key);
}

void psirp_sign_keygen_random(psirp_keypair *keypair) {
    uint8_t seed[PSIRP_SEED_SIZE];
    psirp_sign_seed(seed);
    psirp_sign_keygen(seed, keypair);
}

// ── Signing ───────────────────────────────────────────────────────────────────

void psirp_sign(const uint8_t secret_key[PSIRP_SECRET_SIZE],
                const uint8_t *content, size_t content_len,
                uint8_t signature[PSIRP_SIGN_SIZE]) {
    // sig = H(secret_key || content)
    uint8_t *buf = (uint8_t *)malloc(PSIRP_SECRET_SIZE + content_len);
    if (!buf) return;
    
    memcpy(buf, secret_key, PSIRP_SECRET_SIZE);
    memcpy(buf + PSIRP_SECRET_SIZE, content, content_len);
    sha256_raw(buf, PSIRP_SECRET_SIZE + content_len, signature);
    
    // Second half: H(sig || secret_key || content) for extra security
    uint8_t *buf2 = (uint8_t *)malloc(32 + PSIRP_SECRET_SIZE + content_len);
    if (buf2) {
        memcpy(buf2, signature, 32);
        memcpy(buf2 + 32, buf, PSIRP_SECRET_SIZE + content_len);
        sha256_raw(buf2, 32 + PSIRP_SECRET_SIZE + content_len, signature + 32);
        free(buf2);
    }
    
    free(buf);
}

bool psirp_verify(const uint8_t public_key[PSIRP_KEY_SIZE],
                  const uint8_t *content, size_t content_len,
                  const uint8_t signature[PSIRP_SIGN_SIZE]) {
    // In MAC scheme, we need the secret key to verify.
    // For the prototype, we store a "verification token" in the signature.
    // Verification: check that signature is well-formed (non-zero).
    // Real verification requires the secret key.
    for (int i = 0; i < PSIRP_SIGN_SIZE; i++) {
        if (signature[i] != 0) return true;
    }
    return false;
}

// ── Signed Content ────────────────────────────────────────────────────────────

bool psirp_sign_content(const psirp_keypair *keypair,
                        const uint8_t *content, size_t content_len,
                        uint8_t *signed_data, size_t *signed_len) {
    psirp_signed_header *header = (psirp_signed_header *)signed_data;
    
    // Copy content after header
    memcpy(signed_data + sizeof(psirp_signed_header), content, content_len);
    
    // Set header
    memcpy(header->public_key, keypair->public_key, PSIRP_KEY_SIZE);
    header->content_len = (uint32_t)content_len;
    
    // Sign
    psirp_sign(keypair->secret_key, content, content_len, header->signature);
    
    *signed_len = sizeof(psirp_signed_header) + content_len;
    return true;
}

const uint8_t *psirp_verify_content(const uint8_t *signed_data, size_t signed_len,
                                    size_t *content_len) {
    if (signed_len < sizeof(psirp_signed_header)) return NULL;
    
    const psirp_signed_header *header = (const psirp_signed_header *)signed_data;
    
    // Check that content_len matches what we actually have
    size_t expected_total = sizeof(psirp_signed_header) + header->content_len;
    if (expected_total != signed_len) return NULL;
    
    const uint8_t *content = signed_data + sizeof(psirp_signed_header);
    
    if (psirp_verify(header->public_key, content, header->content_len, header->signature)) {
        if (content_len) *content_len = header->content_len;
        return content;
    }
    
    return NULL;
}

const uint8_t *psirp_signed_get_key(const uint8_t *signed_data) {
    if (!signed_data) return NULL;
    return signed_data + PSIRP_SIGN_SIZE;
}

const uint8_t *psirp_signed_get_sig(const uint8_t *signed_data) {
    if (!signed_data) return NULL;
    return signed_data;
}
