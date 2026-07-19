/**
 * @file dht.h
 * @brief Kademlia-style DHT for content-holder discovery.
 *
 * The DHT maps a content NAME HASH to the set of peers that currently hold
 * (have published/cached) that content. It does NOT store the content itself —
 * only pointers to holders. Resolution flow:
 *
 *   1. A node wants /site/page.html  -> computes name hash H
 *   2. dht_lookup(H) returns the k closest known peers in XOR space
 *   3. Those peers are asked (via mesh Interest) for the content
 *   4. If they don't have it, they forward the lookup to even-closer peers
 *
 * Routing metric is XOR distance: distance(a,b) = a ^ b (as 64-bit ints, since
 * PSIRP name hashes are 64-bit). Each node keeps a routing table bucketed by
 * the bit-length of the distance to known peers (classic Kademlia k-buckets).
 */

#ifndef PSIRP_DHT_H
#define PSIRP_DHT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DHT_ID_BITS      64
#define DHT_K            8      ///< bucket size (replication factor)
#define DHT_MAX_PEERS    (DHT_K * DHT_ID_BITS)  ///< generous upper bound
#define DHT_MAX_HOLDERS  DHT_K

/** @brief A peer entry in the DHT. */
typedef struct {
    uint64_t id;            ///< peer's DHT id (derived from its address/key)
    uint32_t ip;            ///< IPv4 (network order)
    uint16_t port;          ///< UDP port (network order)
    uint8_t  pubkey[32];    ///< optional Ed25519 pubkey of the peer (0 if unknown)
    bool     have_key;      ///< whether pubkey is set
    uint64_t last_seen_ns;  ///< for liveness / eviction
    bool     alive;
} dht_peer;

/** @brief One record: content hash -> holders. */
typedef struct {
    uint64_t content_hash;
    uint32_t holder_ip[DHT_MAX_HOLDERS];
    uint16_t holder_port[DHT_MAX_HOLDERS];
    size_t   holder_count;
    bool     used;
} dht_record;

/** @brief DHT node (routing table + local store). */
typedef struct dht_node {
    uint64_t self_id;                       ///< our DHT id
    dht_peer peers[DHT_MAX_PEERS];          ///< flat peer list (k-bucket flatten)
    size_t   peer_count;
    dht_record records[DHT_K * 64];         ///< local (hash->holders) store
    size_t   record_count;
} dht_node;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void dht_init(dht_node *dht, uint64_t self_id);

/** @brief Derive a stable DHT id from an ip:port pair. */
uint64_t dht_id_from_addr(uint32_t ip, uint16_t port);

// ── Peer management ───────────────────────────────────────────────────────────

/** @brief Add/refresh a peer in the routing table. Returns true if new. */
bool dht_add_peer(dht_node *dht, uint32_t ip, uint16_t port,
                  const uint8_t *pubkey, bool have_key);

/** @brief Find the k closest peers to a target id (XOR nearest first). */
size_t dht_closest_peers(const dht_node *dht, uint64_t target,
                         dht_peer out[], size_t out_max);

// ── Content records ───────────────────────────────────────────────────────────

/** @brief Announce that `ip:port` holds `content_hash` (called on publish/cache). */
void dht_announce(dht_node *dht, uint64_t content_hash, uint32_t ip, uint16_t port);

/** @brief Store a (hash -> holders) record learned from another peer. */
void dht_put_record(dht_node *dht, const dht_record *rec);

/** @brief Look up holders for a content hash.
 *  @return number of holders written to out_ip/out_port (0 if unknown locally). */
size_t dht_lookup(dht_node *dht, uint64_t content_hash,
                  uint32_t out_ip[DHT_MAX_HOLDERS], uint16_t out_port[DHT_MAX_HOLDERS]);

/** @brief Whole-record fetch (for forwarding lookups to other peers). */
const dht_record *dht_get_record(const dht_node *dht, uint64_t content_hash);

// ── Routing helper ────────────────────────────────────────────────────────────

/** @brief XOR distance between two 64-bit ids. */
static inline uint64_t dht_distance(uint64_t a, uint64_t b) { return a ^ b; }

#ifdef __cplusplus
}
#endif

#endif // PSIRP_DHT_H
