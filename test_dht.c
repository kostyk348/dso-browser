/**
 * @file test_dht.c
 * @brief DHT unit tests: peer routing, announce/lookup, record propagation.
 */

#include "dht.h"
#include "signing.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("  %-38s ", name); } while(0)
#define PASS() do { tests_passed++; printf("[PASS]\n"); } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); } while(0)

/* CHECK always evaluates expr (unlike assert under -DNDEBUG) and fails gracefully. */
#define CHECK(expr) do { \
    if (!(expr)) { FAIL("check failed: " #expr); return; } \
} while (0)

static void test_id_stable(void) {
    TEST("id_from_addr_stable");
    uint64_t a = dht_id_from_addr(inet_addr("10.0.0.5"), 8000);
    uint64_t b = dht_id_from_addr(inet_addr("10.0.0.5"), 8000);
    CHECK(a == b);
    uint64_t c = dht_id_from_addr(inet_addr("10.0.0.6"), 8000);
    CHECK(a != c);
    PASS();
}

static void test_peer_add(void) {
    TEST("peer_add_and_refresh");
    dht_node dht;
    dht_init(&dht, 0x1111);
    bool is_new = dht_add_peer(&dht, inet_addr("127.0.0.1"), 9000, NULL, false);
    CHECK(is_new);
    CHECK(dht.peer_count == 1);
    /* refresh should NOT add a second entry */
    is_new = dht_add_peer(&dht, inet_addr("127.0.0.1"), 9000, NULL, false);
    CHECK(!is_new);
    CHECK(dht.peer_count == 1);
    PASS();
}

static void test_closest_peers(void) {
    TEST("closest_peers_xor_order");
    dht_node dht;
    dht_init(&dht, 0);
    /* target id = 0, so closest peer is the one with smallest id */
    dht_add_peer(&dht, inet_addr("10.0.0.1"), 1000, NULL, false);
    dht_add_peer(&dht, inet_addr("10.0.0.2"), 1000, NULL, false);
    dht_add_peer(&dht, inet_addr("10.0.0.3"), 1000, NULL, false);

    dht_peer out[8];
    size_t n = dht_closest_peers(&dht, 0, out, 8);
    CHECK(n == 3);
    /* ids are sorted ascending by XOR distance to 0 == ascending id */
    for (size_t i = 1; i < n; i++)
        CHECK(out[i-1].id <= out[i].id);
    PASS();
}

static void test_announce_lookup(void) {
    TEST("announce_and_lookup");
    dht_node dht;
    dht_init(&dht, 0x2222);
    uint64_t h = 0xABCD1234ABCD1234ULL;
    dht_announce(&dht, h, inet_addr("192.168.1.7"), 7000);

    uint32_t ips[DHT_MAX_HOLDERS];
    uint16_t ports[DHT_MAX_HOLDERS];
    size_t n = dht_lookup(&dht, h, ips, ports);
    CHECK(n == 1);
    CHECK(ips[0] == inet_addr("192.168.1.7"));
    CHECK(ports[0] == 7000);

    /* Unknown hash -> 0 holders */
    CHECK(dht_lookup(&dht, 0xDEAD, ips, ports) == 0);
    PASS();
}

static void test_record_propagate(void) {
    TEST("record_put_propagates");
    dht_node a, b;
    dht_init(&a, 1); dht_init(&b, 2);
    uint64_t h = 0x9999ULL;
    dht_announce(&a, h, inet_addr("1.2.3.4"), 5000);

    const dht_record *rec = dht_get_record(&a, h);
    CHECK(rec && rec->holder_count == 1);

    /* b learns the record from a */
    dht_put_record(&b, rec);
    uint32_t ips[DHT_MAX_HOLDERS];
    uint16_t ports[DHT_MAX_HOLDERS];
    size_t n = dht_lookup(&b, h, ips, ports);
    CHECK(n == 1);
    CHECK(ips[0] == inet_addr("1.2.3.4"));
    PASS();
}

static void test_multi_holder(void) {
    TEST("multiple_holders");
    dht_node dht;
    dht_init(&dht, 0x3333);
    uint64_t h = 0x5555ULL;
    dht_announce(&dht, h, inet_addr("10.0.0.1"), 1000);
    dht_announce(&dht, h, inet_addr("10.0.0.2"), 1000);
    dht_announce(&dht, h, inet_addr("10.0.0.3"), 1000);
    uint32_t ips[DHT_MAX_HOLDERS];
    uint16_t ports[DHT_MAX_HOLDERS];
    size_t n = dht_lookup(&dht, h, ips, ports);
    CHECK(n == 3);
    PASS();
}

/* ── Ed25519 (replaces old MAC) ─────────────────────────────────────────────── */

static void test_ed25519_sign_verify(void) {
    TEST("ed25519_sign_verify");
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    const char *msg = "PSIRP CONTENT hello";
    uint8_t sig[PSIRP_SIGN_SIZE];
    psirp_sign(kp.secret_key, (const uint8_t *)msg, strlen(msg), sig);
    CHECK(psirp_verify(kp.public_key, (const uint8_t *)msg, strlen(msg), sig));
    PASS();
}

static void test_ed25519_tamper_rejected(void) {
    TEST("ed25519_tamper_rejected");
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    char msg[] = "original content";
    uint8_t sig[PSIRP_SIGN_SIZE];
    psirp_sign(kp.secret_key, (const uint8_t *)msg, strlen(msg), sig);
    /* tamper */
    msg[0] = 'X';
    CHECK(!psirp_verify(kp.public_key, (const uint8_t *)msg, strlen(msg), sig));
    PASS();
}

static void test_ed25519_wrong_key(void) {
    TEST("ed25519_wrong_key_rejected");
    psirp_keypair a, b;
    psirp_sign_keygen_random(&a);
    psirp_sign_keygen_random(&b);
    const char *msg = "shared message";
    uint8_t sig[PSIRP_SIGN_SIZE];
    psirp_sign(a.secret_key, (const uint8_t *)msg, strlen(msg), sig);
    CHECK(!psirp_verify(b.public_key, (const uint8_t *)msg, strlen(msg), sig));
    PASS();
}

static void test_signed_content_roundtrip(void) {
    TEST("signed_content_roundtrip");
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    const uint8_t payload[] = "signed blob";
    uint8_t buf[256];
    size_t slen = sizeof(buf);
    CHECK(psirp_sign_content(&kp, payload, sizeof(payload) - 1, buf, &slen));
    size_t clen = 0;
    const uint8_t *out = psirp_verify_content(buf, slen, &clen);
    CHECK(out && clen == sizeof(payload) - 1);
    CHECK(memcmp(out, payload, clen) == 0);
    PASS();
}

int main(void) {
    printf("DHT + Ed25519 Test Suite\n");
    printf("========================\n\n");

    printf("DHT:\n");
    test_id_stable();
    test_peer_add();
    test_closest_peers();
    test_announce_lookup();
    test_record_propagate();
    test_multi_holder();

    printf("\nEd25519:\n");
    test_ed25519_sign_verify();
    test_ed25519_tamper_rejected();
    test_ed25519_wrong_key();
    test_signed_content_roundtrip();

    printf("\n================\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
