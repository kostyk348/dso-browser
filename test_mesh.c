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
    
    assert(idx1 == 0);
    assert(idx2 == 1);
    assert(ctx.peer_count == 2);
    
    // Find peers
    assert(mesh_find_peer(&ctx, "/server1") == 0);
    assert(mesh_find_peer(&ctx, "/server2") == 1);
    assert(mesh_find_peer(&ctx, "/unknown") == -1);
    
    // Update existing peer
    struct sockaddr_in addr1_new = make_addr("192.168.1.20", 9700);
    int idx1_again = mesh_add_peer(&ctx, "/server1", &addr1_new);
    assert(idx1_again == 0);  // Same index, updated
    assert(ctx.peer_count == 2);  // No new peer
    
    // Remove peer
    mesh_remove_peer(&ctx, 0);
    assert(ctx.peer_count == 1);
    assert(mesh_find_peer(&ctx, "/server1") == -1);
    assert(mesh_find_peer(&ctx, "/server2") == 0);
    
    PASS();
}

static void test_fib_management(void) {
    TEST("FIB add/remove/lookup");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    // Add a peer
    struct sockaddr_in addr = make_addr("192.168.1.10", 9700);
    int peer_idx = mesh_add_peer(&ctx, "/server1", &addr);
    assert(peer_idx == 0);
    
    // Add FIB entries
    psirp_name prefix1 = make_name("/site/css");
    psirp_name prefix2 = make_name("/site/images");
    
    assert(mesh_add_fib(&ctx, &prefix1, 0, 0));
    assert(mesh_add_fib(&ctx, &prefix2, 0, 0));
    assert(ctx.fib_count == 2);
    
    // Lookup
    psirp_name lookup1 = make_name("/site/css/style.css");
    assert(mesh_fib_lookup(&ctx, &lookup1) == 0);
    
    psirp_name lookup2 = make_name("/site/images/logo.png");
    assert(mesh_fib_lookup(&ctx, &lookup2) == 1);
    
    psirp_name lookup3 = make_name("/other/file.txt");
    assert(mesh_fib_lookup(&ctx, &lookup3) == -1);
    
    // Remove FIB entry
    mesh_remove_fib(&ctx, 0);
    assert(ctx.fib_count == 1);
    assert(mesh_fib_lookup(&ctx, &lookup1) == -1);
    assert(mesh_fib_lookup(&ctx, &lookup2) == 0);
    
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
    
    assert(mesh_fib_lookup(&ctx, &test1) == 0);
    assert(mesh_fib_lookup(&ctx, &test2) == 0);
    assert(mesh_fib_lookup(&ctx, &test3) == -1);
    
    PASS();
}

static void test_beacon_format(void) {
    TEST("Beacon format parsing");
    
    // Simulate beacon data
    const char *beacon = "MESH_BEACON|server1|/site;/images";
    size_t len = strlen(beacon);
    
    // Verify header
    assert(len > 12);
    assert(memcmp(beacon, "MESH_BEACON|", 12) == 0);
    
    // Parse name
    const char *name_start = beacon + 12;
    const char *sep = strchr(name_start, '|');
    assert(sep != NULL);
    
    char name[256];
    size_t name_len = sep - name_start;
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';
    assert(strcmp(name, "server1") == 0);
    
    // Parse prefixes
    const char *prefixes = sep + 1;
    assert(strcmp(prefixes, "/site;/images") == 0);
    
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
    assert(removed == 1);
    assert(ctx.peer_count == 0);
    
    PASS();
}

static void test_stats(void) {
    TEST("Mesh statistics");
    
    mesh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.interests_forwarded = 100;
    ctx.data_forwarded = 80;
    
    struct sockaddr_in addr = make_addr("192.168.1.10", 9700);
    mesh_add_peer(&ctx, "/server1", &addr);
    mesh_add_peer(&ctx, "/server2", &addr);
    
    size_t peers, fib_entries;
    uint64_t interests, data;
    mesh_stats(&ctx, &peers, &fib_entries, &interests, &data);
    
    assert(peers == 2);
    assert(fib_entries == 0);
    assert(interests == 100);
    assert(data == 80);
    
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
    
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
