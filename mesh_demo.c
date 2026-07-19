/**
 * @file mesh_demo.c
 * @brief Mesh demo: 3 peers exchanging content via PSIRP.
 *
 * Architecture:
 *   Peer A (/games)  ←→  Peer B (/music)  ←→  Peer C (/docs)
 *         ↑                    ↑                    ↑
 *         └────────────────────┴────────────────────┘
 *                    Deterministic routing
 *
 * Usage:
 *   Terminal 1: ./mesh_demo games 9700 ./content/games
 *   Terminal 2: ./mesh_demo music 9701 ./content/music
 *   Terminal 3: ./mesh_demo docs  9702 ./content/docs
 *
 * Then from Peer A:
 *   mesh> fetch /music/song.mp3
 *   → Routes to Peer B via mesh
 */

#include "psirp.h"
#include "net.h"
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

// ── Globals ───────────────────────────────────────────────────────────────────

static volatile int running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

// ── Peer Config ───────────────────────────────────────────────────────────────

typedef struct {
    const char *name;       ///< Peer name (e.g., "/games")
    uint16_t    port;       ///< UDP port
    const char *content_dir; ///< Content directory
} peer_config;

// ── Mesh Receiver Thread ──────────────────────────────────────────────────────

static void *mesh_recv_thread(void *arg) {
    mesh_ctx *mesh = (mesh_ctx *)arg;
    
    uint8_t buf[65536];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    
    while (running) {
        // Check multicast for beacons
        ssize_t n = recvfrom(mesh->node->mcast_fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            buf[n] = '\0';
            mesh_process_beacon(mesh, buf, n, &from);
        }
    }
    
    return NULL;
}

// ── Command Processing ────────────────────────────────────────────────────────

static void process_command(mesh_ctx *mesh, const char *cmd) {
    char command[256], arg1[256], arg2[256];
    
    if (sscanf(cmd, "%255s %255s %255s", command, arg1, arg2) < 1) return;
    
    if (strcmp(command, "fetch") == 0) {
        // Fetch content from mesh
        psirp_name name;
        psirp_name_init(&name, arg1);
        
        printf("Fetching %s...\n", arg1);
        
        const psirp_cs_entry *entry = mesh_forward_interest(mesh, &name, 4000);
        if (entry) {
            printf("Received %zu bytes:\n", entry->data_len);
            // Print first 100 bytes
            size_t print_len = entry->data_len > 100 ? 100 : entry->data_len;
            fwrite(entry->data, 1, print_len, stdout);
            printf("\n");
        } else {
            printf("Not found or timeout\n");
        }
    }
    else if (strcmp(command, "publish") == 0) {
        // Publish local content
        psirp_name prefix;
        psirp_name_init(&prefix, arg1);
        mesh_add_local_prefix(mesh, &prefix);
        printf("Publishing prefix %s\n", arg1);
    }
    else if (strcmp(command, "peers") == 0) {
        // List peers
        printf("Peers (%zu):\n", mesh->peer_count);
        for (size_t i = 0; i < mesh->peer_count; i++) {
            char name[256];
            mesh_peer_name_to_string(&mesh->peers[i], name, sizeof(name));
            printf("  [%zu] %s %s (last seen %lu ms ago)\n",
                   i, name,
                   mesh->peers[i].state == MESH_PEER_CONNECTED ? "connected" : "discovered",
                   (unsigned long)(mesh_now_ms() - mesh->peers[i].last_seen));
        }
    }
    else if (strcmp(command, "fib") == 0) {
        // List FIB
        printf("FIB (%zu entries):\n", mesh->fib_count);
        for (size_t i = 0; i < mesh->fib_count; i++) {
            char prefix[256];
            psirp_name_to_string(&mesh->fib[i].prefix, prefix, sizeof(prefix));
            printf("  [%zu] %s → peer %zu\n", i, prefix, mesh->fib[i].peer_index);
        }
    }
    else if (strcmp(command, "beacon") == 0) {
        // Send beacon
        mesh_send_beacon(mesh);
        printf("Beacon sent\n");
    }
    else if (strcmp(command, "help") == 0) {
        printf("Commands:\n");
        printf("  fetch <name>      - Fetch content from mesh\n");
        printf("  publish <prefix>  - Publish local prefix\n");
        printf("  peers             - List peers\n");
        printf("  fib               - List FIB entries\n");
        printf("  beacon            - Send beacon\n");
        printf("  help              - Show this help\n");
        printf("  quit              - Exit\n");
    }
    else if (strcmp(command, "quit") == 0) {
        running = 0;
    }
    else {
        printf("Unknown command: %s (try 'help')\n", command);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <peer_name> [port] [content_dir]\n", argv[0]);
        printf("Example: %s /games 9700 ./content/games\n", argv[0]);
        return 1;
    }
    
    peer_config config = {
        .name = argv[1],
        .port = argc > 2 ? (uint16_t)atoi(argv[2]) : 9700,
        .content_dir = argc > 3 ? argv[3] : "./content",
    };
    
    // Setup signal handler
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    printf("=== PSIRP Mesh Peer ===\n");
    printf("Name: %s\n", config.name);
    printf("Port: %u\n", config.port);
    printf("Content: %s\n", config.content_dir);
    printf("\n");
    
    // Initialize PSIRP
    psirp_name local_name;
    psirp_name_init(&local_name, config.name);
    
    psirp_cs cs;
    void *arena_mem = malloc(1024 * 1024);  // 1MB arena
    psirp_cs_init(&cs, arena_mem, 1024 * 1024);
    
    psirp_node node;
    if (!psirp_net_init(&node, config.port, &cs)) {
        fprintf(stderr, "Failed to initialize network node\n");
        free(arena_mem);
        return 1;
    }
    
    psirp_net_start(&node);
    
    // Initialize mesh
    mesh_ctx mesh;
    mesh_init(&mesh, &node, &cs, config.name);
    
    // Add local prefix
    mesh_add_local_prefix(&mesh, &local_name);
    
    printf("Node ready on port %u\n", config.port);
    printf("Type 'help' for commands\n\n");
    
    // Start mesh receiver thread
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, mesh_recv_thread, &mesh);
    
    // Send initial beacon
    mesh_send_beacon(&mesh);
    
    // Interactive loop
    char line[256];
    while (running) {
        printf("%s> ", config.name + 1);  // Skip leading /
        fflush(stdout);
        
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        
        // Remove newline
        line[strcspn(line, "\n")] = '\0';
        
        if (strlen(line) > 0) {
            process_command(&mesh, line);
        }
        
        // Periodic beacon
        static uint64_t last_beacon = 0;
        uint64_t now = mesh_now_ms();
        if (now - last_beacon > MESH_BEACON_INTERVAL) {
            mesh_send_beacon(&mesh);
            mesh_check_timeouts(&mesh, now);
            mesh_fib_prune(&mesh, now);
            last_beacon = now;
        }
    }
    
    // Cleanup
    running = 0;
    pthread_join(recv_tid, NULL);
    
    psirp_net_stop(&node);
    free(arena_mem);
    
    printf("\nPeer %s shut down\n", config.name);
    return 0;
}
