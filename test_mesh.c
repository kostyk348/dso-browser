/**
 * @file test_mesh.c
 * @brief Tests for PSIRP mesh overlay.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include "mesh.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-50s ", name); } while(0)
#define PASS() do { printf("[OK]\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); tests_failed++; } while(0)

/* CHECK always evaluates expr (unlike assert under -DNDEBUG) and fails gracefully. */
#define CHECK(expr) do { \
    if (!(expr)) { FAIL("check failed: " #expr); return; } \
} while (0)

// ── Helpers ───────────────────────────────────────────────────────────────────

static psirp_name make_name(const char *str) {
    psirp_name name;
    psirp_name_init(&name, str);
    return name;
}

static struct sockaddr_in make_addr(const char *ip, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_peer_management(void) {
    TEST("Peer add/remove/find");
    
    // Create minimal mesh context (no real network)
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    struct sockaddr_in addr1 = make_addr("192.168.1.10", 9700);
    struct sockaddr_in addr2 = make_addr("192.168.1.11", 9700);
    
    // Add peers
    int idx1 = mesh_add_peer(&ctx, "/server1", &addr1);
    int idx2 = mesh_add_peer(&ctx, "/server2", &addr2);
    
    CHECK(idx1 == 0);
    CHECK(idx2 == 1);
    CHECK(ctx.peer_count == 2);
    
    // Find peers
    CHECK(mesh_find_peer(&ctx, "/server1") == 0);
    CHECK(mesh_find_peer(&ctx, "/server2") == 1);
    CHECK(mesh_find_peer(&ctx, "/unknown") == -1);
    
    // Update existing peer
    struct sockaddr_in addr1_new = make_addr("192.168.1.20", 9700);
    int idx1_again = mesh_add_peer(&ctx, "/server1", &addr1_new);
    CHECK(idx1_again == 0);  // Same index, updated
    CHECK(ctx.peer_count == 2);  // No new peer
    
    // Remove peer
    mesh_remove_peer(&ctx, 0);
    CHECK(ctx.peer_count == 1);
    CHECK(mesh_find_peer(&ctx, "/server1") == -1);
    CHECK(mesh_find_peer(&ctx, "/server2") == 0);
    
    PASS();
}

static void test_fib_management(void) {
    TEST("FIB add/remove/lookup");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    // Add a peer
    struct sockaddr_in addr = make_addr("192.168.1.10", 9700);
    int peer_idx = mesh_add_peer(&ctx, "/server1", &addr);
    CHECK(peer_idx == 0);
    
    // Add FIB entries
    psirp_name prefix1 = make_name("/site/css");
    psirp_name prefix2 = make_name("/site/images");
    
    CHECK(mesh_add_fib(&ctx, &prefix1, 0, 0));
    CHECK(mesh_add_fib(&ctx, &prefix2, 0, 0));
    CHECK(ctx.fib_count == 2);
    
    // Lookup
    psirp_name lookup1 = make_name("/site/css/style.css");
    CHECK(mesh_fib_lookup(&ctx, &lookup1) == 0);
    
    psirp_name lookup2 = make_name("/site/images/logo.png");
    CHECK(mesh_fib_lookup(&ctx, &lookup2) == 1);
    
    psirp_name lookup3 = make_name("/other/file.txt");
    CHECK(mesh_fib_lookup(&ctx, &lookup3) == -1);
    
    // Remove FIB entry
    mesh_remove_fib(&ctx, 0);
    CHECK(ctx.fib_count == 1);
    CHECK(mesh_fib_lookup(&ctx, &lookup1) == -1);
    CHECK(mesh_fib_lookup(&ctx, &lookup2) == 0);
    
    PASS();
}

static void test_prefix_matching(void) {
    TEST("FIB prefix matching");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    struct sockaddr_in addr = make_addr("192.168.1.10", 9700);
    int peer_idx = mesh_add_peer(&ctx, "/server1", &addr);
    
    // Add prefix with wildcard
    psirp_name prefix = make_name("/site");
    mesh_add_fib(&ctx, &prefix, peer_idx, 0);
    
    // Should match any content under /site
    psirp_name test1 = make_name("/site/page.html");
    psirp_name test2 = make_name("/site/css/style.css");
    psirp_name test3 = make_name("/other/file.txt");
    
    CHECK(mesh_fib_lookup(&ctx, &test1) == 0);
    CHECK(mesh_fib_lookup(&ctx, &test2) == 0);
    CHECK(mesh_fib_lookup(&ctx, &test3) == -1);
    
    PASS();
}

static void test_beacon_format(void) {
    TEST("Beacon format parsing");
    
    // Simulate beacon data
    const char *beacon = "MESH_BEACON|server1|/site;/images";
    size_t len = strlen(beacon);
    
    // Verify header
    CHECK(len > 12);
    CHECK(memcmp(beacon, "MESH_BEACON|", 12) == 0);
    
    // Parse name
    const char *name_start = beacon + 12;
    const char *sep = strchr(name_start, '|');
    CHECK(sep != NULL);
    
    char name[256];
    size_t name_len = sep - name_start;
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';
    CHECK(strcmp(name, "server1") == 0);
    
    // Parse prefixes
    const char *prefixes = sep + 1;
    CHECK(strcmp(prefixes, "/site;/images") == 0);
    
    PASS();
}

static void test_peer_timeout(void) {
    TEST("Peer timeout detection");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    struct sockaddr_in addr = make_addr("192.168.1.10", 9700);
    mesh_add_peer(&ctx, "/server1", &addr);
    
    // Simulate old peer
    ctx.peers[0].last_seen = 1000;
    
    // Check timeout at time 20000 (> 15000 timeout)
    size_t removed = mesh_check_timeouts(&ctx, 20000);
    CHECK(removed == 1);
    CHECK(ctx.peer_count == 0);
    
    PASS();
}

static void test_stats(void) {
    TEST("Mesh statistics");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.interests_forwarded = 100;
    ctx.data_forwarded = 80;
    ctx.multi_hop_routed = 50;
    
    struct sockaddr_in addr = make_addr("192.168.1.10", 9700);
    mesh_add_peer(&ctx, "/server1", &addr);
    mesh_add_peer(&ctx, "/server2", &addr);
    
    size_t peers, fib_entries;
    uint64_t interests, data, multi_hop;
    mesh_stats(&ctx, &peers, &fib_entries, &interests, &data, &multi_hop);
    
    CHECK(peers == 2);
    CHECK(fib_entries == 0);
    CHECK(interests == 100);
    CHECK(data == 80);
    CHECK(multi_hop == 50);
    
    PASS();
}

static void test_interest_dedup(void) {
    TEST("Interest dedup cache");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    // Simulate adding to dedup cache
    ctx.interest_cache[0].nonce = 12345;
    ctx.interest_cache[0].timestamp = mesh_now_ms();
    ctx.interest_cache[0].from_peer = 0;
    ctx.interest_cache_count = 1;
    
    // Check dedup
    bool found = false;
    for (size_t i = 0; i < ctx.interest_cache_count; i++) {
        if (ctx.interest_cache[i].nonce == 12345) {
            found = true;
            break;
        }
    }
    CHECK(found);
    
    // Different nonce not found
    found = false;
    for (size_t i = 0; i < ctx.interest_cache_count; i++) {
        if (ctx.interest_cache[i].nonce == 99999) {
            found = true;
            break;
        }
    }
    CHECK(!found);
    
    PASS();
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("mesh tests:\n");
    
    test_peer_management();
    test_fib_management();
    test_prefix_matching();
    test_beacon_format();
    test_peer_timeout();
    test_stats();
    test_interest_dedup();
    
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
