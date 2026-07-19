/**
 * @file mesh.c
 * @brief PSIRP Mesh Overlay implementation.
 *
 * Protocol:
 * - Beacons: UDP multicast, announce peer + prefixes
 * - Interest forwarding: FIB-based routing
 * - Data: follows reverse path (Interest → Data)
 *
 * Design:
 * - Each peer maintains FIB with prefix → peer mappings
 * - Beacons auto-discover peers and propagate prefixes
 * - Interest forwarding: check FIB, forward to next hop
 * - Data forwarding: reverse path (back to requester)
 */

#include "mesh.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/**
 * @brief Format peer name as string.
 */
static size_t peer_name_to_string(const mesh_peer *peer, char *buf, size_t buf_len) {
    if (peer->name_count == 0) {
        buf[0] = '\0';
        return 0;
    }
    
    size_t pos = 0;
    // Add leading slash
    if (pos < buf_len - 1) buf[pos++] = '/';
    
    for (size_t i = 0; i < peer->name_count && pos < buf_len - 1; i++) {
        if (i > 0 && pos < buf_len - 1) {
            buf[pos++] = '/';
        }
        size_t clen = strlen(peer->name[i]);
        if (clen > buf_len - pos - 1) clen = buf_len - pos - 1;
        memcpy(buf + pos, peer->name[i], clen);
        pos += clen;
    }
    buf[pos] = '\0';
    return pos;
}

/**
 * @brief Parse name string into components.
 */
static size_t parse_name(const char *str, char components[][PSIRP_MAX_COMPONENT], size_t max) {
    if (!str || !components || max == 0) return 0;
    
    size_t count = 0;
    const char *p = str;
    
    // Skip leading slash
    if (*p == '/') p++;
    
    while (*p && count < max) {
        const char *start = p;
        while (*p && *p != '/') p++;
        
        size_t len = p - start;
        if (len >= PSIRP_MAX_COMPONENT) len = PSIRP_MAX_COMPONENT - 1;
        memcpy(components[count], start, len);
        components[count][len] = '\0';
        count++;
        
        if (*p == '/') p++;
    }
    
    return count;
}

// ── Peer Management ───────────────────────────────────────────────────────────

int mesh_add_peer(mesh_ctx *ctx, const char *name, const struct sockaddr_in *addr) {
    if (!ctx || !name || !addr || ctx->peer_count >= MESH_MAX_PEERS) return -1;
    
    // Check if peer already exists
    for (size_t i = 0; i < ctx->peer_count; i++) {
        char existing[256];
        peer_name_to_string(&ctx->peers[i], existing, sizeof(existing));
        if (strcmp(existing, name) == 0) {
            // Update address and timestamp
            ctx->peers[i].addr = *addr;
            ctx->peers[i].last_seen = now_ms();
            ctx->peers[i].state = MESH_PEER_CONNECTED;
            return (int)i;
        }
    }
    
    // Add new peer
    mesh_peer *peer = &ctx->peers[ctx->peer_count];
    memset(peer, 0, sizeof(*peer));
    
    peer->name_count = parse_name(name, peer->name, PSIRP_MAX_COMPONENTS);
    peer->addr = *addr;
    peer->state = MESH_PEER_DISCOVERED;
    peer->last_seen = now_ms();
    
    return (int)ctx->peer_count++;
}

void mesh_remove_peer(mesh_ctx *ctx, size_t peer_index) {
    if (!ctx || peer_index >= ctx->peer_count) return;
    
    // Remove FIB entries for this peer
    for (size_t i = ctx->fib_count; i > 0; i--) {
        if (ctx->fib[i-1].peer_index == peer_index) {
            mesh_remove_fib(ctx, i-1);
        }
    }
    
    // Shift peers
    for (size_t i = peer_index; i < ctx->peer_count - 1; i++) {
        ctx->peers[i] = ctx->peers[i+1];
    }
    ctx->peer_count--;
    
    // Update FIB peer indices
    for (size_t i = 0; i < ctx->fib_count; i++) {
        if (ctx->fib[i].peer_index > peer_index) {
            ctx->fib[i].peer_index--;
        }
    }
}

int mesh_find_peer(const mesh_ctx *ctx, const char *name) {
    if (!ctx || !name) return -1;
    
    for (size_t i = 0; i < ctx->peer_count; i++) {
        char peer_name[256];
        peer_name_to_string(&ctx->peers[i], peer_name, sizeof(peer_name));
        if (strcmp(peer_name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void mesh_peer_seen(mesh_ctx *ctx, size_t peer_index) {
    if (!ctx || peer_index >= ctx->peer_count) return;
    ctx->peers[peer_index].last_seen = now_ms();
    ctx->peers[peer_index].state = MESH_PEER_CONNECTED;
}

size_t mesh_check_timeouts(mesh_ctx *ctx, uint64_t now_ms_val) {
    if (!ctx) return 0;
    
    size_t removed = 0;
    for (size_t i = ctx->peer_count; i > 0; i--) {
        if (now_ms_val - ctx->peers[i-1].last_seen > MESH_PEER_TIMEOUT) {
            mesh_remove_peer(ctx, i-1);
            removed++;
        }
    }
    return removed;
}

// ── FIB Management ────────────────────────────────────────────────────────────

bool mesh_add_fib(mesh_ctx *ctx, const psirp_name *prefix, size_t peer_index,
                  uint32_t lifetime_ms) {
    if (!ctx || !prefix || peer_index >= ctx->peer_count || ctx->fib_count >= MESH_MAX_FIB)
        return false;
    
    // Check if entry already exists
    for (size_t i = 0; i < ctx->fib_count; i++) {
        if (psirp_name_equal(&ctx->fib[i].prefix, prefix) &&
            ctx->fib[i].peer_index == peer_index) {
            // Update lifetime
            ctx->fib[i].lifetime_ms = lifetime_ms;
            ctx->fib[i].created_at = now_ms();
            return true;
        }
    }
    
    // Add new entry
    mesh_fib_entry *entry = &ctx->fib[ctx->fib_count++];
    entry->prefix = *prefix;
    entry->peer_index = peer_index;
    entry->lifetime_ms = lifetime_ms;
    entry->created_at = now_ms();
    
    return true;
}

void mesh_remove_fib(mesh_ctx *ctx, size_t fib_index) {
    if (!ctx || fib_index >= ctx->fib_count) return;
    
    for (size_t i = fib_index; i < ctx->fib_count - 1; i++) {
        ctx->fib[i] = ctx->fib[i+1];
    }
    ctx->fib_count--;
}

int mesh_fib_lookup(const mesh_ctx *ctx, const psirp_name *name) {
    if (!ctx || !name) return -1;
    
    int best_match = -1;
    size_t best_len = 0;
    
    for (size_t i = 0; i < ctx->fib_count; i++) {
        if (psirp_name_is_prefix(&ctx->fib[i].prefix, name)) {
            size_t prefix_len = ctx->fib[i].prefix.count;
            if (prefix_len > best_len) {
                best_len = prefix_len;
                best_match = (int)i;
            }
        }
    }
    
    return best_match;
}

size_t mesh_fib_prune(mesh_ctx *ctx, uint64_t now_ms_val) {
    if (!ctx) return 0;
    
    size_t removed = 0;
    for (size_t i = ctx->fib_count; i > 0; i--) {
        mesh_fib_entry *entry = &ctx->fib[i-1];
        if (entry->lifetime_ms > 0 &&
            now_ms_val - entry->created_at > entry->lifetime_ms) {
            mesh_remove_fib(ctx, i-1);
            removed++;
        }
    }
    return removed;
}

// ── Mesh Operations ───────────────────────────────────────────────────────────

void mesh_init(mesh_ctx *ctx, psirp_node *node, psirp_cs *cs, const char *local_name) {
    if (!ctx) return;
    
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->cs = cs;
    
    // Setup multicast address
    memset(&ctx->mcast_addr, 0, sizeof(ctx->mcast_addr));
    ctx->mcast_addr.sin_family = AF_INET;
    ctx->mcast_addr.sin_port = htons(PSIRP_MULTICAST_PORT);
    inet_pton(AF_INET, "239.255.0.1", &ctx->mcast_addr.sin_addr);
}

bool mesh_send_beacon(mesh_ctx *ctx) {
    if (!ctx || !ctx->node) return false;
    
    // Build beacon: MESH_BEACON|name|prefix1;prefix2;...
    char buf[4096];
    size_t pos = 0;
    
    // Header
    memcpy(buf + pos, "MESH_BEACON|", 12);
    pos += 12;
    
    // Local name
    for (size_t i = 0; i < ctx->local_prefix_count; i++) {
        if (i > 0) {
            buf[pos++] = '/';
        }
        // Use first component as peer name
        if (ctx->local_prefixes[i].count > 0) {
            size_t clen = strlen(ctx->local_prefixes[i].components[0]);
            memcpy(buf + pos, ctx->local_prefixes[i].components[0], clen);
            pos += clen;
            break;  // Use first prefix's first component as name
        }
    }
    
    buf[pos++] = '|';
    
    // Prefixes
    for (size_t i = 0; i < ctx->local_prefix_count; i++) {
        if (i > 0) {
            buf[pos++] = ';';
        }
        // Write prefix as string
        for (size_t j = 0; j < ctx->local_prefixes[i].count; j++) {
            if (j > 0) {
                buf[pos++] = '/';
            }
            size_t clen = strlen(ctx->local_prefixes[i].components[j]);
            if (pos + clen >= sizeof(buf) - 2) break;
            memcpy(buf + pos, ctx->local_prefixes[i].components[j], clen);
            pos += clen;
        }
    }
    
    buf[pos] = '\0';
    
    // Send to all known peers
    for (size_t i = 0; i < ctx->peer_count; i++) {
        if (ctx->peers[i].state == MESH_PEER_CONNECTED) {
            sendto(ctx->node->fd, buf, pos, 0,
                   (struct sockaddr *)&ctx->peers[i].addr,
                   sizeof(ctx->peers[i].addr));
        }
    }
    
    // Send to multicast group
    sendto(ctx->node->mcast_fd, buf, pos, 0,
           (struct sockaddr *)&ctx->mcast_addr,
           sizeof(ctx->mcast_addr));
    
    ctx->beacons_sent++;
    return true;
}

bool mesh_process_beacon(mesh_ctx *ctx, const uint8_t *data, size_t data_len,
                         const struct sockaddr_in *from) {
    if (!ctx || !data || data_len < 12) return false;
    
    // Check header
    if (memcmp(data, "MESH_BEACON|", 12) != 0) return false;
    
    const char *str = (const char *)data + 12;
    const char *sep = strchr(str, '|');
    if (!sep) return false;
    
    // Extract peer name
    char name[256];
    size_t name_len = sep - str;
    if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
    memcpy(name, str, name_len);
    name[name_len] = '\0';
    
    // Add/update peer
    int peer_idx = mesh_add_peer(ctx, name, from);
    if (peer_idx < 0) return false;
    
    mesh_peer_seen(ctx, peer_idx);
    
    // Extract prefixes
    const char *prefixes = sep + 1;
    const char *p = prefixes;
    
    ctx->peers[peer_idx].num_prefixes = 0;
    
    while (*p && ctx->peers[peer_idx].num_prefixes < MESH_MAX_PREFIXES) {
        const char *start = p;
        while (*p && *p != ';') p++;
        
        size_t plen = p - start;
        if (plen > 0) {
            char prefix_str[256];
            if (plen >= sizeof(prefix_str)) plen = sizeof(prefix_str) - 1;
            memcpy(prefix_str, start, plen);
            prefix_str[plen] = '\0';
            
            size_t idx = ctx->peers[peer_idx].num_prefixes;
            ctx->peers[peer_idx].prefix_count[idx] = parse_name(
                prefix_str,
                ctx->peers[peer_idx].prefixes[idx],
                PSIRP_MAX_COMPONENTS
            );
            ctx->peers[peer_idx].num_prefixes++;
            
            // Add FIB entry for this prefix
            psirp_name fib_prefix;
            psirp_name_init(&fib_prefix, prefix_str);
            mesh_add_fib(ctx, &fib_prefix, peer_idx, MESH_BEACON_INTERVAL * 3);
        }
        
        if (*p == ';') p++;
    }
    
    ctx->beacons_received++;
    return true;
}

const psirp_cs_entry *mesh_forward_interest(mesh_ctx *ctx, const psirp_name *name,
                                            uint32_t timeout_ms) {
    if (!ctx || !name || !ctx->node) return NULL;
    
    // First check local content store
    const psirp_cs_entry *local = psirp_cs_lookup(ctx->cs, name);
    if (local) return local;
    
    // Forward via FIB
    int fib_idx = mesh_fib_lookup(ctx, name);
    if (fib_idx < 0) return NULL;
    
    mesh_fib_entry *entry = &ctx->fib[fib_idx];
    mesh_peer *peer = &ctx->peers[entry->peer_index];
    
    if (peer->state != MESH_PEER_CONNECTED) return NULL;
    
    // Send interest to peer
    const psirp_cs_entry *result = psirp_net_fetch(ctx->node, name, timeout_ms);
    
    ctx->interests_forwarded++;
    if (result) {
        ctx->data_forwarded++;
    }
    
    return result;
}

void mesh_add_local_prefix(mesh_ctx *ctx, const psirp_name *prefix) {
    if (!ctx || !prefix || ctx->local_prefix_count >= MESH_MAX_PREFIXES) return;
    
    ctx->local_prefixes[ctx->local_prefix_count++] = *prefix;
}

void mesh_stats(const mesh_ctx *ctx, size_t *peers, size_t *fib_entries,
                uint64_t *interests, uint64_t *data) {
    if (!ctx) return;
    
    if (peers) *peers = ctx->peer_count;
    if (fib_entries) *fib_entries = ctx->fib_count;
    if (interests) *interests = ctx->interests_forwarded;
    if (data) *data = ctx->data_forwarded;
}
