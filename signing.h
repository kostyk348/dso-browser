/**
 * @file signing.h
 * @brief Content signing with Ed25519.
 *
 * Provides:
 * - Key generation
 * - Content signing
 * - Content verification
 * - Signed content format
 *
 * Ed25519 chosen for:
 * - Fast (10k signs/sec)
 * - Small keys (32 bytes)
 * - Small signatures (64 bytes)
 * - No nonce needed (deterministic)
 */

#ifndef PSIRP_SIGNING_H
#define PSIRP_SIGNING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define PSIRP_KEY_SIZE      32    ///< Ed25519 public key size
#define PSIRP_SECRET_SIZE   32    ///< Ed25519 secret key size
#define PSIRP_SIGN_SIZE     64    ///< Ed25519 signature size
#define PSIRP_SEED_SIZE     32    ///< Seed size for key generation

// ── Key Types ─────────────────────────────────────────────────────────────────

/** @brief Ed25519 keypair. */
typedef struct {
    uint8_t public_key[PSIRP_KEY_SIZE];    ///< Public key
    uint8_t secret_key[PSIRP_SECRET_SIZE]; ///< Secret key (expanded)
} psirp_keypair;

/** @brief Signed content header. */
typedef struct {
    uint8_t  signature[PSIRP_SIGN_SIZE];   ///< Ed25519 signature
    uint8_t  public_key[PSIRP_KEY_SIZE];   ///< Signer's public key
    uint32_t content_len;                  ///< Original content length
} psirp_signed_header;

// ── Key Generation ────────────────────────────────────────────────────────────

/**
 * @brief Generate random seed.
 *
 * @param seed  Output seed (32 bytes)
 */
void psirp_sign_seed(uint8_t seed[PSIRP_SEED_SIZE]);

/**
 * @brief Generate keypair from seed.
 *
 * @param seed      Input seed (32 bytes)
 * @param keypair   Output keypair
 */
void psirp_sign_keygen(const uint8_t seed[PSIRP_SEED_SIZE], psirp_keypair *keypair);

/**
 * @brief Generate random keypair.
 *
 * @param keypair   Output keypair
 */
void psirp_sign_keygen_random(psirp_keypair *keypair);

// ── Signing ───────────────────────────────────────────────────────────────────

/**
 * @brief Sign content.
 *
 * @param secret_key    Secret key (32 bytes)
 * @param content       Content to sign
 * @param content_len   Content length
 * @param signature     Output signature (64 bytes)
 */
void psirp_sign(const uint8_t secret_key[PSIRP_SECRET_SIZE],
                const uint8_t *content, size_t content_len,
                uint8_t signature[PSIRP_SIGN_SIZE]);

/**
 * @brief Verify signature.
 *
 * @param public_key    Public key (32 bytes)
 * @param content       Content to verify
 * @param content_len   Content length
 * @param signature     Signature to verify (64 bytes)
 * @return true if signature is valid
 */
bool psirp_verify(const uint8_t public_key[PSIRP_KEY_SIZE],
                  const uint8_t *content, size_t content_len,
                  const uint8_t signature[PSIRP_SIGN_SIZE]);

// ── Signed Content ────────────────────────────────────────────────────────────

/**
 * @brief Create signed content.
 *
 * Output format:
 *   [psirp_signed_header][original content]
 *
 * @param keypair       Keypair (for signing)
 * @param content       Content to sign
 * @param content_len   Content length
 * @param signed_data   Output buffer (must be large enough)
 * @param signed_len    Output length
 * @return true on success
 */
bool psirp_sign_content(const psirp_keypair *keypair,
                        const uint8_t *content, size_t content_len,
                        uint8_t *signed_data, size_t *signed_len);

/**
 * @brief Verify signed content.
 *
 * @param signed_data   Signed content (header + content)
 * @param signed_len    Signed content length
 * @param content_len   Output: original content length
 * @return Pointer to content, or NULL if invalid
 */
const uint8_t *psirp_verify_content(const uint8_t *signed_data, size_t signed_len,
                                    size_t *content_len);

/**
 * @brief Get public key from signed content.
 */
const uint8_t *psirp_signed_get_key(const uint8_t *signed_data);

/**
 * @brief Get signature from signed content.
 */
const uint8_t *psirp_signed_get_sig(const uint8_t *signed_data);

#ifdef __cplusplus
}
#endif

#endif // PSIRP_SIGNING_H
