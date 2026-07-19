/**
 * @file net.h
 * @brief PSIRP Network Transport — UDP-based Interest/Data exchange.
 *
 * Provides:
 * - UDP socket management (unicast + multicast)
 * - Interest/Data packet send/receive
 * - Content store integration (auto-reply from CS)
 * - Peer discovery via multicast
 */

#ifndef PSIRP_NET_H
#define PSIRP_NET_H

#include "psirp.h"
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define PSIRP_DEFAULT_PORT     9400   ///< Default UDP port
#define PSIRP_MULTICAST_GROUP  "239.255.0.1"  ///< Multicast group for discovery
#define PSIRP_MULTICAST_PORT   9401   ///< Multicast port
#define PSIRP_MAX_PEERS        64     ///< Max known peers
#define PSIRP_RECV_BUF_SIZE    (1 << 20)  ///< 1MB receive buffer
#define PSIRP_INTEREST_TIMEOUT_MS  4000   ///< Default interest lifetime

// ── Network Node ──────────────────────────────────────────────────────────────

/** @brief Pending interest — tracks outstanding request.
 *  `active` is written by the receiver thread and read by the caller's
 *  wait loop, so it must be `volatile` to survive -O3 reordering. */
typedef struct {
    psirp_name  name;           ///< What we requested
    uint64_t    nonce;          ///< Request nonce
    uint64_t    sent_time_ns;   ///< When we sent it
    uint32_t    lifetime_ms;    ///< Timeout
    volatile bool active;       ///< Still waiting? (cross-thread)
} psirp_pending;

/** @brief Network node — UDP socket + content store + FIB. */
typedef struct {
    int             fd;             ///< UDP socket
    int             mcast_fd;       ///< Multicast socket
    uint16_t        port;           ///< Our port
    psirp_cs       *cs;             ///< Content store (external)
    psirp_fib       fib;            ///< Forwarding table

    // Pending interests
    psirp_pending   pending[256];   ///< Outstanding interests
    volatile size_t pending_count;  ///< Active pending count (cross-thread)

    // Known peers
    psirp_peer      peers[PSIRP_MAX_PEERS];
    volatile size_t peer_count;     ///< Cross-thread (beacon discovery)

    // Threading
    pthread_t       recv_thread;    ///< Receiver thread
    volatile bool   running;        ///< Thread control (cross-thread)

    // Callbacks
    void (*on_data)(const psirp_data *data, void *user);
    void *on_data_user;

    // Statistics
    volatile uint64_t interests_sent;
    volatile uint64_t data_received;
    volatile uint64_t data_sent;
    volatile uint64_t interests_forwarded;
} psirp_node;

/**
 * @brief Initialize network node.
 * @param node  Node to initialize
 * @param port  UDP port (0 = default)
 * @param cs    Content store to use
 */
bool psirp_net_init(psirp_node *node, uint16_t port, psirp_cs *cs);

/**
 * @brief Start receiver thread.
 */
bool psirp_net_start(psirp_node *node);

/**
 * @brief Stop receiver thread and close sockets.
 */
void psirp_net_stop(psirp_node *node);

/**
 * @brief Send interest packet to a peer.
 * @param node     Network node
 * @param name     Content name to request
 * @param peer     Peer to send to (NULL = use FIB)
 * @param timeout_ms  Timeout (0 = default)
 * @return Nonce of the interest (for dedup)
 */
uint64_t psirp_net_interest(psirp_node *node, const psirp_name *name,
                            const psirp_peer *peer, uint32_t timeout_ms);

/**
 * @brief Send data packet to a peer.
 */
bool psirp_net_data(psirp_node *node, const psirp_peer *peer,
                    const psirp_data *data);

/**
 * @brief Add peer manually.
 */
bool psirp_net_add_peer(psirp_node *node, uint32_t ip, uint16_t port);

/**
 * @brief Broadcast interest on multicast (for discovery/content request).
 */
bool psirp_net_multicast_interest(psirp_node *node, const psirp_name *name);

// ── Content Fetch (blocking) ──────────────────────────────────────────────────

/**
 * @brief Fetch content by name (blocking).
 *
 * Tries local CS first, then sends interest to peers.
 * Returns pointer to data in content store.
 *
 * @param node     Network node
 * @param name     Content name
 * @param timeout_ms  Max wait time
 * @return Pointer to CS entry, or NULL if not found
 */
const psirp_cs_entry *psirp_net_fetch(psirp_node *node, const psirp_name *name,
                                      uint32_t timeout_ms);

// ── Content Publish ───────────────────────────────────────────────────────────

/**
 * @brief Publish content locally and announce to peers.
 */
bool psirp_net_publish(psirp_node *node, const psirp_name *name,
                       const uint8_t *data, size_t data_len,
                       uint32_t freshness_ms);

#ifdef __cplusplus
}
#endif

#endif // PSIRP_NET_H
