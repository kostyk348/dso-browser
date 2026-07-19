/**
 * @file test_psirp.c
 * @brief PSIRP unit + integration tests.
 *
 * Tests: naming, serialization, content store, FIB, network exchange.
 */

#include "psirp.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %-40s ", name); } while(0)
#define PASS() do { tests_passed++; printf("[PASS]\n"); } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); } while(0)

// ── Name Tests ────────────────────────────────────────────────────────────────

static void test_name_init(void) {
    TEST("name_init");
    psirp_name a, b;
    assert(psirp_name_init(&a, "/test/page.html"));
    assert(a.count == 2);
    assert(strcmp(a.components[0], "test") == 0);
    assert(strcmp(a.components[1], "page.html") == 0);
    assert(a.hash != 0);
    psirp_name_init(&b, "/test/page.html");
    assert(a.hash == b.hash);
    PASS();
}

static void test_name_equal(void) {
    TEST("name_equal");
    psirp_name a, b, c;
    psirp_name_init(&a, "/foo/bar");
    psirp_name_init(&b, "/foo/bar");
    psirp_name_init(&c, "/foo/baz");
    assert(psirp_name_equal(&a, &b));
    assert(!psirp_name_equal(&a, &c));
    PASS();
}

static void test_name_prefix(void) {
    TEST("name_is_prefix");
    psirp_name full, prefix, other;
    psirp_name_init(&full, "/site/page/resource");
    psirp_name_init(&prefix, "/site/page");
    psirp_name_init(&other, "/other");
    assert(psirp_name_is_prefix(&prefix, &full));
    assert(!psirp_name_is_prefix(&other, &full));
    PASS();
}

static void test_name_to_string(void) {
    TEST("name_to_string");
    psirp_name name;
    psirp_name_init(&name, "/hello/world");
    char buf[64];
    size_t len = psirp_name_to_string(&name, buf, sizeof(buf));
    assert(len == 12);
    assert(strcmp(buf, "/hello/world") == 0);
    PASS();
}

// ── Serialization Tests ───────────────────────────────────────────────────────

static void test_interest_roundtrip(void) {
    TEST("interest_serialize_roundtrip");
    psirp_name name;
    psirp_name_init(&name, "/test/page.html");
    psirp_interest orig = { .name = name, .nonce = 123456789ULL, .lifetime_ms = 4000 };
    uint8_t buf[4096];
    size_t len = psirp_interest_serialize(&orig, buf, sizeof(buf));
    assert(len > 0);
    psirp_interest decoded;
    assert(psirp_interest_deserialize(&decoded, buf, len));
    assert(decoded.nonce == 123456789ULL);
    assert(decoded.lifetime_ms == 4000);
    assert(decoded.name.count == 2);
    assert(decoded.name.hash == name.hash);
    PASS();
}

static void test_data_roundtrip(void) {
    TEST("data_serialize_roundtrip");
    psirp_name name;
    psirp_name_init(&name, "/site/page");
    const char *content = "<html><body>Hello</body></html>";
    psirp_data orig = {
        .name = name, .data = (const uint8_t *)content,
        .data_len = strlen(content), .timestamp = 1000000000ULL, .freshness_ms = 60000
    };
    uint8_t buf[4096];
    size_t len = psirp_data_serialize(&orig, buf, sizeof(buf));
    assert(len > 0);
    psirp_data decoded;
    assert(psirp_data_deserialize(&decoded, buf, len));
    assert(decoded.timestamp == 1000000000ULL);
    assert(decoded.data_len == strlen(content));
    assert(decoded.name.hash == name.hash);
    PASS();
}

// ── Content Store Tests ───────────────────────────────────────────────────────

static void test_cs_store_lookup(void) {
    TEST("cs_store_and_lookup");
    uint8_t arena[64 * 1024];
    psirp_cs cs;
    psirp_cs_init(&cs, arena, sizeof(arena));
    psirp_name name;
    psirp_name_init(&name, "/test/content");
    const char *data = "hello world";
    assert(psirp_cs_store(&cs, &name, (const uint8_t *)data, strlen(data), 0));
    const psirp_cs_entry *entry = psirp_cs_lookup(&cs, &name);
    assert(entry != NULL);
    assert(entry->data_len == strlen(data));
    assert(memcmp(entry->data, data, strlen(data)) == 0);
    PASS();
}

static void test_cs_miss(void) {
    TEST("cs_miss");
    uint8_t arena[64 * 1024];
    psirp_cs cs;
    psirp_cs_init(&cs, arena, sizeof(arena));
    psirp_name name;
    psirp_name_init(&name, "/not/present");
    assert(psirp_cs_lookup(&cs, &name) == NULL);
    PASS();
}

static void test_fib(void) {
    TEST("fib_lookup");
    psirp_fib fib = {0};
    assert(psirp_fib_add(&fib, "/site", inet_addr("10.0.0.1"), 9400));
    psirp_name name;
    psirp_name_init(&name, "/site/page");
    const psirp_peer *peer = psirp_fib_lookup(&fib, &name);
    assert(peer != NULL);
    assert(peer->ip == inet_addr("10.0.0.1"));
    PASS();
}

// ── Network Exchange Test ─────────────────────────────────────────────────────

static void test_network_exchange(void) {
    TEST("network_interest_data_exchange");
    
    // Publisher node
    uint8_t cs_a_mem[64 * 1024];
    psirp_cs cs_a;
    psirp_cs_init(&cs_a, cs_a_mem, sizeof(cs_a_mem));
    psirp_node node_a;
    assert(psirp_net_init(&node_a, 9700, &cs_a));
    
    psirp_name pub_name;
    psirp_name_init(&pub_name, "/server/data");
    const char *payload = "PSIRP CONTENT";
    psirp_cs_store(&cs_a, &pub_name, (const uint8_t *)payload, strlen(payload), 0);
    
    assert(psirp_net_start(&node_a));
    
    // Client node
    uint8_t cs_b_mem[64 * 1024];
    psirp_cs cs_b;
    psirp_cs_init(&cs_b, cs_b_mem, sizeof(cs_b_mem));
    psirp_node node_b;
    assert(psirp_net_init(&node_b, 0, &cs_b));
    psirp_net_add_peer(&node_b, inet_addr("127.0.0.1"), 9700);
    assert(psirp_net_start(&node_b));
    
    usleep(200000); // Let threads start
    
    // Fetch
    psirp_name req;
    psirp_name_init(&req, "/server/data");
    const psirp_cs_entry *entry = psirp_net_fetch(&node_b, &req, 3000);
    
    if (entry && entry->data_len == strlen(payload) &&
        memcmp(entry->data, payload, strlen(payload)) == 0) {
        PASS();
    } else {
        FAIL("content mismatch or not found");
    }
    
    psirp_net_stop(&node_b);
    psirp_net_stop(&node_a);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("PSIRP Test Suite\n");
    printf("================\n\n");
    
    printf("Naming:\n");
    test_name_init();
    test_name_equal();
    test_name_prefix();
    test_name_to_string();
    
    printf("\nSerialization:\n");
    test_interest_roundtrip();
    test_data_roundtrip();
    
    printf("\nContent Store:\n");
    test_cs_store_lookup();
    test_cs_miss();
    
    printf("\nFIB:\n");
    test_fib();
    
    printf("\nNetwork:\n");
    test_network_exchange();
    
    printf("\n================\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);
    
    return tests_passed == tests_run ? 0 : 1;
}
