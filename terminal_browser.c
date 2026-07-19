/**
 * @file terminal_browser.c
 * @brief Terminal-based PSIRP browser — no GTK4/WebKitGTK required.
 *
 * Features:
 * - Content-name addressing (/site/page.html)
 * - HTML parsing and text display
 * - Sub-resource fetching (CSS/JS/images)
 * - Mesh integration
 * - Interactive commands
 *
 * Usage:
 *   ./terminal_browser
 *   > /test/page.html
 *   > fetch /site/css/style.css
 *   > peers
 *   > quit
 */

#include "psirp.h"
#include "net.h"
#include "mesh.h"
#include "html_parse.h"
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

// ── State ─────────────────────────────────────────────────────────────────────

typedef struct {
    psirp_node     node;
    psirp_cs       cs;
    mesh_ctx       mesh;
    void          *arena_mem;
    
    // Current page
    char           current_page[256];
    uint8_t       *page_content;
    size_t         page_len;
    
    // Parsed resources
    html_parse_result html_result;
} app_state;

// ── HTML to Text ──────────────────────────────────────────────────────────────

/**
 * @brief Strip HTML tags and display as text.
 */
static void display_html_text(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    
    const char *html = (const char *)data;
    int in_tag = 0;
    int in_style = 0;
    int in_script = 0;
    
    for (size_t i = 0; i < len; i++) {
        char c = html[i];
        
        if (c == '<') {
            // Check for tags to skip
            if (strncasecmp(html + i, "<style", 6) == 0) in_style = 1;
            else if (strncasecmp(html + i, "<script", 7) == 0) in_script = 1;
            else if (strncasecmp(html + i, "</style", 7) == 0) in_style = 0;
            else if (strncasecmp(html + i, "</script", 8) == 0) in_script = 0;
            
            in_tag = 1;
            continue;
        }
        
        if (c == '>') {
            in_tag = 0;
            // Add newline after block elements
            if (strncasecmp(html + i - 4, "<br", 3) == 0 ||
                strncasecmp(html + i - 5, "<div", 4) == 0 ||
                strncasecmp(html + i - 6, "<p>", 3) == 0) {
                putchar('\n');
            }
            continue;
        }
        
        if (!in_tag && !in_style && !in_script) {
            putchar(c);
        }
    }
    putchar('\n');
}

// ── Commands ──────────────────────────────────────────────────────────────────

static void cmd_fetch(app_state *app, const char *name) {
    psirp_name psirp_name;
    psirp_name_init(&psirp_name, name);
    
    printf("Fetching %s...\n", name);
    
    const psirp_cs_entry *entry = mesh_forward_interest(&app->mesh, &psirp_name,
                                                        MESH_INTEREST_TTL,
                                                        mesh_now_ms(),
                                                        -1, 4000);
    if (entry) {
        printf("Received %zu bytes\n", entry->data_len);
        
        // Store as current page
        strncpy(app->current_page, name, sizeof(app->current_page) - 1);
        
        // Parse HTML
        html_parse((const char *)entry->data, entry->data_len, name, &app->html_result);
        
        // Display content
        printf("\n=== %s ===\n", name);
        printf("Title: %.*s\n", (int)app->html_result.title_len, 
               app->html_result.title ? app->html_result.title : "(none)");
        printf("---\n");
        display_html_text(entry->data, entry->data_len);
        printf("---\n");
        
        // Show sub-resources
        if (app->html_result.resource_count > 0) {
            printf("\nSub-resources (%zu):\n", app->html_result.resource_count);
            for (size_t i = 0; i < app->html_result.resource_count; i++) {
                html_resource *res = &app->html_result.resources[i];
                const char *type_str = "unknown";
                switch (res->type) {
                    case HTML_RES_STYLESHEET: type_str = "CSS"; break;
                    case HTML_RES_SCRIPT:     type_str = "JS"; break;
                    case HTML_RES_IMAGE:      type_str = "IMG"; break;
                    case HTML_RES_LINK:       type_str = "LINK"; break;
                    case HTML_RES_INLINE_CSS: type_str = "INLINE"; break;
                    case HTML_RES_META:       type_str = "META"; break;
                }
                printf("  [%s] %s\n", type_str, res->name);
            }
        }
    } else {
        printf("Not found or timeout\n");
    }
}

static void cmd_peers(app_state *app) {
    printf("Peers (%zu):\n", app->mesh.peer_count);
    for (size_t i = 0; i < app->mesh.peer_count; i++) {
        char name[256];
        mesh_peer_name_to_string(&app->mesh.peers[i], name, sizeof(name));
        printf("  [%zu] %s %s (seen %lu ms ago)\n",
               i, name,
               app->mesh.peers[i].state == MESH_PEER_CONNECTED ? "connected" : "?",
               (unsigned long)(mesh_now_ms() - app->mesh.peers[i].last_seen));
    }
}

static void cmd_fib(app_state *app) {
    printf("FIB (%zu entries):\n", app->mesh.fib_count);
    for (size_t i = 0; i < app->mesh.fib_count; i++) {
        char prefix[256];
        psirp_name_to_string(&app->mesh.fib[i].prefix, prefix, sizeof(prefix));
        printf("  [%zu] %s → peer %zu\n", i, prefix, app->mesh.fib[i].peer_index);
    }
}

static void cmd_help(void) {
    printf("Commands:\n");
    printf("  <name>              Fetch content by PSIRP name\n");
    printf("  fetch <name>        Fetch content by PSIRP name\n");
    printf("  peers               List connected peers\n");
    printf("  fib                 Show forwarding table\n");
    printf("  beacon              Send beacon\n");
    printf("  help                Show this help\n");
    printf("  quit                Exit\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    const char *name = argc > 1 ? argv[1] : "/browser";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 9800;
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    printf("=== PSIRP Terminal Browser ===\n");
    printf("Name: %s\n", name);
    printf("Port: %u\n", port);
    printf("Type 'help' for commands\n\n");
    
    // Initialize
    app_state app;
    memset(&app, 0, sizeof(app));
    
    app.arena_mem = malloc(1024 * 1024);  // 1MB arena
    psirp_cs_init(&app.cs, app.arena_mem, 1024 * 1024);
    
    if (!psirp_net_init(&app.node, port, &app.cs)) {
        fprintf(stderr, "Failed to init network\n");
        free(app.arena_mem);
        return 1;
    }
    psirp_net_start(&app.node);
    
    mesh_init(&app.mesh, &app.node, &app.cs, name);
    
    // Send initial beacon
    psirp_name local_prefix;
    psirp_name_init(&local_prefix, name);
    mesh_add_local_prefix(&app.mesh, &local_prefix);
    mesh_send_beacon(&app.mesh);
    
    // Interactive loop
    char line[256];
    while (running) {
        printf("psirp> ");
        fflush(stdout);
        
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        line[strcspn(line, "\n")] = '\0';
        
        if (strlen(line) == 0) continue;
        
        // Parse command
        char cmd[64], arg[256];
        if (sscanf(line, "%63s %255s", cmd, arg) >= 2) {
            if (strcmp(cmd, "fetch") == 0) {
                cmd_fetch(&app, arg);
            } else if (strcmp(cmd, "peers") == 0) {
                cmd_peers(&app);
            } else if (strcmp(cmd, "fib") == 0) {
                cmd_fib(&app);
            } else if (strcmp(cmd, "beacon") == 0) {
                mesh_send_beacon(&app.mesh);
                printf("Beacon sent\n");
            } else if (strcmp(cmd, "help") == 0) {
                cmd_help();
            } else if (strcmp(cmd, "quit") == 0) {
                running = 0;
            } else if (cmd[0] == '/') {
                // Direct fetch by name
                cmd_fetch(&app, line);
            } else {
                printf("Unknown command: %s (try 'help')\n", cmd);
            }
        } else if (line[0] == '/') {
            // Direct fetch by name
            cmd_fetch(&app, line);
        } else if (strcmp(line, "peers") == 0) {
            cmd_peers(&app);
        } else if (strcmp(line, "fib") == 0) {
            cmd_fib(&app);
        } else if (strcmp(line, "beacon") == 0) {
            mesh_send_beacon(&app.mesh);
            printf("Beacon sent\n");
        } else if (strcmp(line, "help") == 0) {
            cmd_help();
        } else if (strcmp(line, "quit") == 0) {
            running = 0;
        } else {
            printf("Unknown command: %s (try 'help')\n", line);
        }
        
        // Periodic beacon
        static uint64_t last_beacon = 0;
        uint64_t now = mesh_now_ms();
        if (now - last_beacon > MESH_BEACON_INTERVAL) {
            mesh_send_beacon(&app.mesh);
            mesh_check_timeouts(&app.mesh, now);
            mesh_fib_prune(&app.mesh, now);
            last_beacon = now;
        }
    }
    
    // Cleanup
    psirp_net_stop(&app.node);
    free(app.arena_mem);
    
    printf("\nBrowser shut down\n");
    return 0;
}
