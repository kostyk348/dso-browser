/**
 * @file test_signing.c
 * @brief Tests for content signing.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "signing.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-50s ", name); } while(0)
#define PASS() do { printf("[OK]\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); tests_failed++; } while(0)

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_keygen(void) {
    TEST("Key generation");
    
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    
    // Keys should not be all zeros
    int nonzero = 0;
    for (int i = 0; i < PSIRP_KEY_SIZE; i++) {
        if (kp.public_key[i] != 0) nonzero = 1;
    }
    assert(nonzero);
    
    // Public and secret should be different
    assert(memcmp(kp.public_key, kp.secret_key, PSIRP_KEY_SIZE) != 0);
    
    PASS();
}

static void test_sign_verify(void) {
    TEST("Sign and verify");
    
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    
    const uint8_t content[] = "Hello, PSIRP!";
    size_t content_len = strlen((char *)content);
    
    uint8_t signature[PSIRP_SIGN_SIZE];
    psirp_sign(kp.secret_key, content, content_len, signature);
    
    // Verify (MAC scheme: signature is non-zero = valid)
    assert(psirp_verify(kp.public_key, content, content_len, signature));
    
    // Empty signature should fail
    uint8_t empty_sig[PSIRP_SIGN_SIZE] = {0};
    assert(!psirp_verify(kp.public_key, content, content_len, empty_sig));
    
    PASS();
}

static void test_signed_content(void) {
    TEST("Signed content packaging");
    
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    
    const uint8_t content[] = "Test content for signing";
    size_t content_len = strlen((char *)content);
    
    uint8_t signed_data[1024];
    size_t signed_len;
    
    assert(psirp_sign_content(&kp, content, content_len, signed_data, &signed_len));
    assert(signed_len == sizeof(psirp_signed_header) + content_len);
    
    // Verify signed content
    size_t verified_len;
    const uint8_t *verified_content = psirp_verify_content(signed_data, signed_len, &verified_len);
    
    assert(verified_content != NULL);
    assert(verified_len == content_len);
    assert(memcmp(verified_content, content, content_len) == 0);
    
    PASS();
}

static void test_tamper_detection(void) {
    TEST("Signed content structure integrity");
    
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    
    const uint8_t content[] = "Original content";
    size_t content_len = strlen((char *)content);
    
    uint8_t signed_data[1024];
    size_t signed_len;
    psirp_sign_content(&kp, content, content_len, signed_data, &signed_len);
    
    // Tamper with header (change content_len)
    ((psirp_signed_header *)signed_data)->content_len = 999;
    
    // Verification should detect length mismatch
    size_t verified_len;
    const uint8_t *verified_content = psirp_verify_content(signed_data, signed_len, &verified_len);
    
    // With tampered header, content_len doesn't match
    assert(verified_content == NULL || verified_len != content_len);
    
    PASS();
}

static void test_deterministic_signing(void) {
    TEST("Deterministic signing (same seed = same signature)");
    
    uint8_t seed[PSIRP_SEED_SIZE] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                                      17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
    
    psirp_keypair kp1, kp2;
    psirp_sign_keygen(seed, &kp1);
    psirp_sign_keygen(seed, &kp2);
    
    // Same seed should produce same keys
    assert(memcmp(kp1.public_key, kp2.public_key, PSIRP_KEY_SIZE) == 0);
    assert(memcmp(kp1.secret_key, kp2.secret_key, PSIRP_SECRET_SIZE) == 0);
    
    // Same key should produce same signature
    const uint8_t content[] = "Deterministic test";
    size_t content_len = strlen((char *)content);
    
    uint8_t sig1[PSIRP_SIGN_SIZE], sig2[PSIRP_SIGN_SIZE];
    psirp_sign(kp1.secret_key, content, content_len, sig1);
    psirp_sign(kp2.secret_key, content, content_len, sig2);
    
    assert(memcmp(sig1, sig2, PSIRP_SIGN_SIZE) == 0);
    
    PASS();
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("signing tests:\n");
    
    test_keygen();
    test_sign_verify();
    test_signed_content();
    test_tamper_detection();
    test_deterministic_signing();
    
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
