/**
 * @file test_dynamic.c
 * @brief Dynamic-content features: versioned names, chunking, LRU cache,
 *        pub/sub, compute-on-peer.
 */

#include "psirp.h"
#include "dht.h"
#include "pubsub.h"
#include "compute.h"
#include "signing.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("  %-40s ", name); } while(0)
#define PASS() do { tests_passed++; printf("[PASS]\n"); } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); } while(0)

/* CHECK evaluates expr ALWAYS (even under -DNDEBUG) and records failure
 * instead of aborting. CHECK() is unsafe here: with -DNDEBUG it drops the
 * call entirely, including calls with required side effects. */
#define CHECK(expr) do { \
    if (!(expr)) { FAIL("check failed: " #expr); return; } \
} while (0)

/* ── Versioned names ──────────────────────────────────────────────────────── */

static void test_versioned_name(void) {
    TEST("versioned_name_parse");
    psirp_name n;
    CHECK(psirp_name_init(&n, "/news@42"));
    CHECK(n.has_version);
    CHECK(n.version == 42);
    CHECK(n.count == 1);
    CHECK(strcmp(n.components[0], "news") == 0);
    char buf[64];
    psirp_name_to_string(&n, buf, sizeof(buf));
    CHECK(strcmp(buf, "/news@42") == 0);
    PASS();
}

static void test_versioned_cs_latest(void) {
    TEST("cs_lookup_latest_version");
    psirp_cs cs;
    psirp_cs_init(&cs, NULL, 0);

    psirp_name v1, v2;
    psirp_name_init(&v1, "/feed@1");
    psirp_name_init(&v2, "/feed@2");
    CHECK(psirp_cs_store(&cs, &v1, (const uint8_t*)"one", 3, 0));
    CHECK(psirp_cs_store(&cs, &v2, (const uint8_t*)"two", 3, 0));

    psirp_name latest;
    psirp_name_init(&latest, "/feed");   /* no version -> newest */
    const psirp_cs_entry *e = psirp_cs_lookup(&cs, &latest);
    CHECK(e && e->data_len == 3);
    CHECK(memcmp(e->data, "two", 3) == 0);
    PASS();
}

/* ── Chunking ──────────────────────────────────────────────────────────────── */

static void test_chunking_small(void) {
    TEST("chunking_small_stored_direct");
    psirp_cs cs;
    psirp_cs_init(&cs, NULL, 0);
    psirp_name n;
    psirp_name_init(&n, "/page.html");
    const char *body = "<html>small</html>";
    CHECK(psirp_cs_store_chunked(&cs, &n, (const uint8_t*)body, strlen(body), 0));
    uint8_t out[1024]; size_t len = 0;
    CHECK(psirp_cs_lookup_chunked(&cs, &n, out, sizeof(out), &len));
    CHECK(len == strlen(body));
    CHECK(memcmp(out, body, len) == 0);
    PASS();
}

static void test_chunking_large(void) {
    TEST("chunking_large_reassemble");
    psirp_cs cs;
    psirp_cs_init(&cs, NULL, 0);
    psirp_name n;
    psirp_name_init(&n, "/big.bin");
    size_t total = PSIRP_CHUNK_SIZE * 3 + 12345; /* > 3 chunks */
    uint8_t *data = (uint8_t *)malloc(total);
    CHECK(data);
    for (size_t i = 0; i < total; i++) data[i] = (uint8_t)(i * 31 + 7);
    CHECK(psirp_cs_store_chunked(&cs, &n, data, total, 0));
    uint8_t *out = (uint8_t *)malloc(total);
    size_t len = 0;
    CHECK(psirp_cs_lookup_chunked(&cs, &n, out, total, &len));
    CHECK(len == total);
    CHECK(memcmp(out, data, total) == 0);
    free(data); free(out);
    PASS();
}

/* ── LRU byte-budgeted cache ───────────────────────────────────────────────── */

static void test_lru_eviction(void) {
    TEST("lru_byte_budget_evicts");
    psirp_cs cs;
    psirp_cs_init(&cs, NULL, 0);
    psirp_cs_set_budget(&cs, 100);  /* ~5 small objects fit */

    for (int i = 0; i < 20; i++) {
        char name[32], val[64];
        snprintf(name, sizeof(name), "/obj%d", i);
        snprintf(val, sizeof(val), "payload-%d-padding", i); /* ~20 bytes each */
        psirp_name n;
        psirp_name_init(&n, name);
        CHECK(psirp_cs_store(&cs, &n, (const uint8_t*)val, strlen(val), 0));
    }
    /* Budget 100 / ~20 bytes => only a few fit; older ones evicted. */
    CHECK(cs.bytes_used <= 100 + 64); /* small slack */
    /* The oldest objects should have been evicted; newest present. */
    psirp_name newest;
    psirp_name_init(&newest, "/obj19");
    CHECK(psirp_cs_lookup(&cs, &newest) != NULL);
    psirp_name oldest;
    psirp_name_init(&oldest, "/obj0");
    CHECK(psirp_cs_lookup(&cs, &oldest) == NULL); /* evicted as LRU */
    PASS();
}

/* ── Pub/Sub ──────────────────────────────────────────────────────────────── */

static void test_pubsub(void) {
    TEST("pubsub_publish_poll");
    dht_node dht;
    dht_init(&dht, 0xABC);
    pubsub_ctx ps;
    pubsub_init(&ps, &dht, 127 << 24 | 1, 9000);

    psirp_name topic;
    psirp_name_init(&topic, "/news");
    pubsub_subscribe(&ps, &topic);

    /* No version yet */
    uint64_t v = 0;
    CHECK(!pubsub_poll(&ps, &topic, &v));

    /* Publisher pushes v1 then v2 */
    CHECK(pubsub_publish(&ps, &topic, 1));
    CHECK(pubsub_publish(&ps, &topic, 2));
    CHECK(!pubsub_publish(&ps, &topic, 2)); /* not newer */
    CHECK(!pubsub_publish(&ps, &topic, 1)); /* older */

    CHECK(pubsub_poll(&ps, &topic, &v));
    CHECK(v == 2);

    /* Build versioned name and confirm it resolves */
    psirp_name vn;
    CHECK(pubsub_versioned_name(&topic, v, &vn));
    char buf[64];
    psirp_name_to_string(&vn, buf, sizeof(buf));
    CHECK(strcmp(buf, "/news@2") == 0);
    PASS();
}

/* ── Compute-on-peer ──────────────────────────────────────────────────────── */

static size_t echo_handler(const uint8_t *params, size_t plen, uint8_t *out, size_t cap) {
    if (plen > cap) return 0;
    memcpy(out, params, plen);
    return plen;
}

static size_t render_handler(const uint8_t *params, size_t plen, uint8_t *out, size_t cap) {
    /* Simulate a DB-backed render: params = user id, output = page w/ timestamp */
    int n = snprintf((char *)out, cap, "PAGE for user %.*s @ %lu",
                     (int)plen, (const char *)params, (unsigned long)time(NULL));
    return (size_t)n;
}

static void test_compute(void) {
    TEST("compute_execute_and_verify");
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);

    psirp_compute_req req;
    memset(&req, 0, sizeof(req));
    psirp_name_init(&req.name, "/render");
    const char *uid = "alice";
    memcpy(req.params, uid, strlen(uid));
    req.params_len = strlen(uid);
    req.nonce = 12345;

    psirp_compute_resp resp;
    CHECK(psirp_compute_execute(&req, render_handler, &kp, &resp));
    CHECK(psirp_compute_verify(&resp));

    /* Tamper -> verify fails */
    resp.result[0] ^= 0xFF;
    CHECK(!psirp_compute_verify(&resp));
    PASS();
}

static void test_compute_serialize(void) {
    TEST("compute_req_resp_serialize");
    psirp_keypair kp;
    psirp_sign_keygen_random(&kp);
    psirp_compute_req req;
    memset(&req, 0, sizeof(req));
    psirp_name_init(&req.name, "/api/calc");
    memcpy(req.params, "x=1", 3);
    req.params_len = 3;

    uint8_t buf[512];
    size_t n = psirp_compute_req_serialize(&req, buf, sizeof(buf));
    CHECK(n > 0);
    psirp_compute_req req2;
    CHECK(psirp_compute_req_deserialize(&req2, buf, n));

    psirp_compute_resp resp;
    CHECK(psirp_compute_execute(&req, echo_handler, &kp, &resp));
    uint8_t buf2[2048];
    size_t m = psirp_compute_resp_serialize(&resp, buf2, sizeof(buf2));
    CHECK(m > 0);
    psirp_compute_resp resp2;
    CHECK(psirp_compute_resp_deserialize(&resp2, buf2, m));
    CHECK(psirp_compute_verify(&resp2));
    PASS();
}

int main(void) {
    printf("Dynamic Content Test Suite\n");
    printf("============================\n\n");

    printf("Versioned names:\n");
    test_versioned_name();
    test_versioned_cs_latest();

    printf("\nChunking:\n");
    test_chunking_small();
    test_chunking_large();

    printf("\nLRU cache:\n");
    test_lru_eviction();

    printf("\nPub/Sub:\n");
    test_pubsub();

    printf("\nCompute-on-peer:\n");
    test_compute();
    test_compute_serialize();

    printf("\n================\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
