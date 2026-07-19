/**
 * @file client.cpp
 * @brief PSIRP Client — Fetch content by name.
 *
 * Usage:
 *   ./client <name> [peer_ip] [peer_port]
 *
 * Fetches content by name and prints it to stdout.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "psirp.h"
#include "net.h"
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <content_name> [peer_ip] [peer_port]\n", argv[0]);
        fprintf(stderr, "Example: %s /test/page\n", argv[0]);
        fprintf(stderr, "Example: %s /site/page 192.168.1.100 9400\n", argv[0]);
        return 1;
    }
    
    const char *name_str = argv[1];
    const char *peer_ip = argc > 2 ? argv[2] : "127.0.0.1";
    uint16_t peer_port = argc > 3 ? (uint16_t)atoi(argv[3]) : PSIRP_DEFAULT_PORT;
    
    printf("PSIRP Client\n");
    printf("Content name: %s\n", name_str);
    printf("Peer: %s:%u\n", peer_ip, peer_port);
    printf("\n");
    
    // Initialize content store
    uint8_t cs_arena_mem[4 * 1024 * 1024]; // 4MB
    psirp_cs cs;
    psirp_cs_init(&cs, cs_arena_mem, sizeof(cs_arena_mem));
    
    // Initialize network
    psirp_node net;
    if (!psirp_net_init(&net, 0, &cs)) { // port 0 = ephemeral
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }
    
    // Add peer
    psirp_net_add_peer(&net, inet_addr(peer_ip), peer_port);
    
    // Start receiver
    if (!psirp_net_start(&net)) {
        fprintf(stderr, "Failed to start receiver\n");
        return 1;
    }
    
    // Parse content name
    psirp_name name;
    if (!psirp_name_init(&name, name_str)) {
        fprintf(stderr, "Invalid content name: %s\n", name_str);
        return 1;
    }
    
    printf("Fetching content...\n");
    
    // Fetch content
    const psirp_cs_entry *entry = psirp_net_fetch(&net, &name, 5000);
    
    if (!entry) {
        fprintf(stderr, "Content not found: %s\n", name_str);
        psirp_net_stop(&net);
        return 1;
    }
    
    // Print content info
    printf("Found: %s\n", name_str);
    printf("Size: %zu bytes\n", entry->data_len);
    printf("Timestamp: %lu\n", (unsigned long)entry->timestamp);
    printf("Freshness: %u ms\n", entry->freshness_ms);
    printf("\n");
    
    // Print content
    printf("--- Content ---\n");
    fwrite(entry->data, 1, entry->data_len, stdout);
    printf("\n--- End ---\n");
    
    // Cleanup
    psirp_net_stop(&net);
    
    return 0;
}
