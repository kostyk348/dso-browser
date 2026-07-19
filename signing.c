/**
 * @file signing.c
 * @brief Content signing — real Ed25519 (libsodium crypto_sign_ed25519).
 *
 * Replaces the old symmetric-MAC prototype with actual asymmetric
 * signatures:
 *   - psirp_sign()    produces a 64-byte Ed25519 signature over the content
 *   - psirp_verify()  verifies the signature against the signer's public key
 *
 * The public API in signing.h is unchanged, so existing callers (publisher,
 * client, browser, tests) keep working. The only behavioural change is that
 * psirp_verify() now performs a real cryptographic check instead of the old
 * "signature is non-zero" stub.
 *
 * Backend: libsodium (crypto_sign_ed25519). For the PS Vita port, define
 * PSIRP_SIGN_REF10 and drop in a tiny ref10 implementation; the API stays the
 * same.
 */

#include "signing.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#ifdef PSIRP_SIGN_REF10
/* Vita / no-libsodium builds provide these elsewhere. */
extern int psirp_ed25519_sign(uint8_t sig[64], const uint8_t sk[32],
                              const uint8_t *msg, size_t len);
extern int psirp_ed25519_verify(const uint8_t sig[64], const uint8_t pk[32],
                                const uint8_t *msg, size_t len);
#else
#include <sodium.h>
#endif

// ── Randomness ────────────────────────────────────────────────────────────────

void psirp_sign_seed(uint8_t seed[PSIRP_SEED_SIZE]) {
#ifndef PSIRP_SIGN_REF10
    if (sodium_init() < 0) { /* not fatal here; fall through to urandom */ }
    randombytes_buf(seed, PSIRP_SEED_SIZE);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, seed, PSIRP_SEED_SIZE); close(fd); }
#endif
}

// ── Key Generation ────────────────────────────────────────────────────────────

void psirp_sign_keygen(const uint8_t seed[PSIRP_SEED_SIZE], psirp_keypair *keypair) {
#ifndef PSIRP_SIGN_REF10
    if (sodium_init() < 0) return;
    crypto_sign_ed25519_seed_keypair(keypair->public_key, keypair->secret_key, seed);
#else
    psirp_ed25519_keypair(keypair->public_key, keypair->secret_key, seed);
#endif
}

void psirp_sign_keygen_random(psirp_keypair *keypair) {
#ifndef PSIRP_SIGN_REF10
    if (sodium_init() < 0) return;
    crypto_sign_ed25519_keypair(keypair->public_key, keypair->secret_key);
#else
    uint8_t seed[PSIRP_SEED_SIZE];
    psirp_sign_seed(seed);
    psirp_ed25519_keypair(keypair->public_key, keypair->secret_key, seed);
#endif
}

// ── Signing ───────────────────────────────────────────────────────────────────

void psirp_sign(const uint8_t secret_key[PSIRP_SECRET_SIZE],
                const uint8_t *content, size_t content_len,
                uint8_t signature[PSIRP_SIGN_SIZE]) {
#ifndef PSIRP_SIGN_REF10
    if (sodium_init() < 0) { memset(signature, 0, PSIRP_SIGN_SIZE); return; }
    /* crypto_sign_detached produces a raw 64-byte signature (no detached msg). */
    crypto_sign_ed25519_detached(signature, NULL, content, content_len, secret_key);
#else
    psirp_ed25519_sign(signature, secret_key, content, content_len);
#endif
}

bool psirp_verify(const uint8_t public_key[PSIRP_KEY_SIZE],
                  const uint8_t *content, size_t content_len,
                  const uint8_t signature[PSIRP_SIGN_SIZE]) {
#ifndef PSIRP_SIGN_REF10
    if (sodium_init() < 0) return false;
    return crypto_sign_ed25519_verify_detached(signature, content, content_len,
                                               public_key) == 0;
#else
    return psirp_ed25519_verify(signature, public_key, content, content_len) == 0;
#endif
}

// ── Signed Content ────────────────────────────────────────────────────────────

bool psirp_sign_content(const psirp_keypair *keypair,
                        const uint8_t *content, size_t content_len,
                        uint8_t *signed_data, size_t *signed_len) {
    if (!signed_data || !signed_len) return false;
    if (*signed_len < sizeof(psirp_signed_header) + content_len) return false;

    psirp_signed_header *header = (psirp_signed_header *)signed_data;

    memcpy(signed_data + sizeof(psirp_signed_header), content, content_len);

    memcpy(header->public_key, keypair->public_key, PSIRP_KEY_SIZE);
    header->content_len = (uint32_t)content_len;

    psirp_sign(keypair->secret_key, content, content_len, header->signature);

    *signed_len = sizeof(psirp_signed_header) + content_len;
    return true;
}

const uint8_t *psirp_verify_content(const uint8_t *signed_data, size_t signed_len,
                                    size_t *content_len) {
    if (signed_len < sizeof(psirp_signed_header)) return NULL;

    const psirp_signed_header *header = (const psirp_signed_header *)signed_data;

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
