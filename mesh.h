/**
 * @file mesh.h
 * @brief PSIRP Mesh Overlay — Multi-peer content routing.
 *
 * Architecture:
 * - Each peer has a name, address, and content store
 * - Interests are forwarded via FIB (Forwarding Information Base)
 * - Data follows reverse path (Interest → Data)
 * - Peer discovery via multicast beacons
 *
 * Protocol:
 * - Unicast Interest/Data exchange (existing net.c)
 * - Mesh beacon: "I am /peer_name at IP:PORT, I have /prefix1, /prefix2"
 * - FIB entries: "for /prefix, forward to peer X"
 */

#ifndef MESH_H
#define MESH_H

#include "psirp.h"
#include "net.h"
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define MESH_MAX_PEERS      32    ///< Max peers in mesh
#define MESH_MAX_FIB        128   ///< Max FIB entries
#define MESH_MAX_PREFIXES   16    ///< Max prefixes per peer
#define MESH_BEACON_INTERVAL 5000 ///< Beacon interval (ms)
#define MESH_PEER_TIMEOUT   15000 ///< Peer timeout (ms)

// ── Peer ──────────────────────────────────────────────────────────────────────

/** @brief Mesh peer state. */
typedef enum {
    MESH_PEER_UNKNOWN,
    MESH_PEER_DISCOVERED,   ///< Discovered via beacon
    MESH_PEER_CONNECTED,    ///< Active connection
    MESH_PEER_DEAD          ///< Timed out
} mesh_peer_state;

/** @brief Mesh peer. */
typedef struct {
    char            name[PSIRP_MAX_COMPONENTS][PSIRP_MAX_COMPONENT]; ///< Peer name
    size_t          name_count;                        ///< Name components
    struct sockaddr_in addr;                           ///< Peer address
    mesh_peer_state state;                             ///< Peer state
    uint64_t        last_seen;                         ///< Last beacon time (ms)
    char            prefixes[MESH_MAX_PREFIXES][PSIRP_MAX_COMPONENTS][PSIRP_MAX_COMPONENT]; ///< Content prefixes
    size_t          prefix_count[MESH_MAX_PREFIXES];   ///< Prefix component counts
    size_t          num_prefixes;                      ///< Number of prefixes
} mesh_peer;

// ── FIB Entry ─────────────────────────────────────────────────────────────────

/** @brief FIB entry — route to peer. */
typedef struct {
    psirp_name  prefix;             ///< Content prefix to match
    size_t      peer_index;         ///< Peer index in mesh
    uint32_t    lifetime_ms;        ///< Entry lifetime (0 = permanent)
    uint64_t    created_at;         ///< Creation time (ms)
} mesh_fib_entry;

// ── Mesh Context ──────────────────────────────────────────────────────────────

/** @brief Mesh overlay context. */
typedef struct {
    psirp_node     *node;           ///< Network node
    psirp_cs       *cs;             ///< Content store

    // Peers
    mesh_peer       peers[MESH_MAX_PEERS];
    size_t          peer_count;

    // FIB
    mesh_fib_entry  fib[MESH_MAX_FIB];
    size_t          fib_count;

    // Local prefixes (what we publish)
    psirp_name      local_prefixes[MESH_MAX_PREFIXES];
    size_t          local_prefix_count;

    // Multicast
    struct sockaddr_in mcast_addr;  ///< Multicast group address
    int              mcast_fd;      ///< Multicast socket

    // Stats
    uint64_t        interests_forwarded;
    uint64_t        data_forwarded;
    uint64_t        beacons_sent;
    uint64_t        beacons_received;
} mesh_ctx;

// ── Peer Management ───────────────────────────────────────────────────────────

/**
 * @brief Add a peer to the mesh.
 *
 * @param ctx       Mesh context
 * @param name      Peer name (e.g., "/server1")
 * @param addr      Peer address
 * @return Peer index, or -1 on failure
 */
int mesh_add_peer(mesh_ctx *ctx, const char *name, const struct sockaddr_in *addr);

/**
 * @brief Remove a peer from the mesh.
 */
void mesh_remove_peer(mesh_ctx *ctx, size_t peer_index);

/**
 * @brief Find peer by name.
 *
 * @return Peer index, or -1 if not found
 */
int mesh_find_peer(const mesh_ctx *ctx, const char *name);

/**
 * @brief Update peer's last seen time.
 */
void mesh_peer_seen(mesh_ctx *ctx, size_t peer_index);

/**
 * @brief Check for timed-out peers, mark as dead.
 *
 * @param now_ms    Current time in milliseconds
 * @return Number of peers removed
 */
size_t mesh_check_timeouts(mesh_ctx *ctx, uint64_t now_ms);

// ── FIB Management ────────────────────────────────────────────────────────────

/**
 * @brief Add FIB entry.
 *
 * @param ctx           Mesh context
 * @param prefix        Content prefix (e.g., "/site/css")
 * @param peer_index    Peer to forward to
 * @param lifetime_ms   Entry lifetime (0 = permanent)
 * @return true on success
 */
bool mesh_add_fib(mesh_ctx *ctx, const psirp_name *prefix, size_t peer_index,
                  uint32_t lifetime_ms);

/**
 * @brief Remove FIB entry.
 */
void mesh_remove_fib(mesh_ctx *ctx, size_t fib_index);

/**
 * @brief Look up FIB for content name.
 *
 * @return Peer index to forward to, or -1 if not found
 */
int mesh_fib_lookup(const mesh_ctx *ctx, const psirp_name *name);

/**
 * @brief Prune expired FIB entries.
 *
 * @param now_ms    Current time in milliseconds
 * @return Number of entries removed
 */
size_t mesh_fib_prune(mesh_ctx *ctx, uint64_t now_ms);

// ── Mesh Operations ───────────────────────────────────────────────────────────

/**
 * @brief Initialize mesh context.
 *
 * @param ctx       Mesh context
 * @param node      Network node
 * @param cs        Content store
 * @param local_name Local peer name (e.g., "/client1")
 */
void mesh_init(mesh_ctx *ctx, psirp_node *node, psirp_cs *cs, const char *local_name);

/**
 * @brief Send mesh beacon (announce local prefixes).
 *
 * Beacon format: MESH_BEACON|name|prefix1,prefix2,...
 *
 * @return true on success
 */
bool mesh_send_beacon(mesh_ctx *ctx);

/**
 * @brief Process received beacon.
 *
 * @param data      Beacon data
 * @param data_len  Data length
 * @param from      Sender address
 * @return true if beacon was processed
 */
bool mesh_process_beacon(mesh_ctx *ctx, const uint8_t *data, size_t data_len,
                         const struct sockaddr_in *from);

/**
 * @brief Forward interest to next hop via FIB.
 *
 * @param ctx       Mesh context
 * @param name      Content name
 * @param timeout_ms Fetch timeout
 * @return Content entry, or NULL if not found
 */
const psirp_cs_entry *mesh_forward_interest(mesh_ctx *ctx, const psirp_name *name,
                                            uint32_t timeout_ms);

/**
 * @brief Add local prefix (what this peer publishes).
 */
void mesh_add_local_prefix(mesh_ctx *ctx, const psirp_name *prefix);

/**
 * @brief Get mesh statistics.
 */
void mesh_stats(const mesh_ctx *ctx, size_t *peers, size_t *fib_entries,
                uint64_t *interests, uint64_t *data);

#ifdef __cplusplus
}
#endif

#endif // MESH_H
