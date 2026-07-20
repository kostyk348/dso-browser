/**
 * @file psirp.h
 * @brief PSIRP — Publish-Subscribe Internet Routing Prototype
 *
 * Content-centric networking layer for DSO.
 * Content is addressed by hierarchical names, not IP addresses.
 *
 * Flow:
 *   1. Create content name: /org/page/section/resource
 *   2. Publisher: psirp_publish(name, data, len)
 *   3. Subscriber: psirp_interest(name) → returns data
 *   4. Content store: arena-backed cache by name hash
 */

#ifndef PSIRP_H
#define PSIRP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define PSIRP_MAX_COMPONENTS  16    ///< Max path components per name
#define PSIRP_MAX_COMPONENT   64    ///< Max length per component
#define PSIRP_MAX_NAME        (PSIRP_MAX_COMPONENTS * PSIRP_MAX_COMPONENT)
#define PSIRP_MAX_DATA        (1 << 20)  ///< 1MB max content size
#define PSIRP_CHUNK_SIZE      (64 * 1024)  ///< Chunk size for large content
#define PSIRP_CS_MAX_ENTRIES  4096   ///< Max entries in content store
#define PSIRP_NONCE_RANDOM    1      ///< Use random nonce

// ── Content Name ──────────────────────────────────────────────────────────────

/**
 * @brief Hierarchical content name.
 *
 * Example: /github/user/repo/README.md
 * Components: ["github", "user", "repo", "README.md"]
 *
 * Optional version: a trailing "@<uint64>" on the LAST component marks a
 * specific version, e.g. "/news@42". A name without @version matches the
 * newest version stored (used by pub/sub "latest" semantics).
 */
typedef struct {
    const char *components[PSIRP_MAX_COMPONENTS]; ///< Path components
    size_t      count;                             ///< Number of components
    uint64_t    hash;                              ///< FNV-1a hash for fast lookup
    uint64_t    version;                           ///< Version (0 = unversioned/latest)
    bool        has_version;                       ///< Whether @version was present
} psirp_name;

/**
 * @brief Initialize name from path string.
 * @param name  Output name struct
 * @param path  Slash-separated path (e.g., "/site/page/resource")
 * @return true on success
 */
bool psirp_name_init(psirp_name *name, const char *path);

/**
 * @brief Initialize name from components array.
 */
bool psirp_name_init_components(psirp_name *name, const char **components, size_t count);

/**
 * @brief Compare two names for equality.
 */
bool psirp_name_equal(const psirp_name *a, const psirp_name *b);

/**
 * @brief Check if name a is prefix of name b.
 */
bool psirp_name_is_prefix(const psirp_name *prefix, const psirp_name *name);

/**
 * @brief Get name as string. Writes to provided buffer.
 * @return Number of characters written (excluding null terminator)
 */
size_t psirp_name_to_string(const psirp_name *name, char *buf, size_t buf_len);

// ── Packets ───────────────────────────────────────────────────────────────────

/** @brief Packet types. */
typedef enum {
    PSIRP_PKT_INTEREST,   ///< Request for content by name
    PSIRP_PKT_DATA,       ///< Response with content
    PSIRP_PKT_PUBLISH,    ///< Announce new content / peer
    PSIRP_PKT_CANCEL,     ///< Cancel pending interest
    PSIRP_PKT_NOTIFY,     ///< Pub/Sub: new version available
    PSIRP_PKT_COMPUTE_REQ,///< Compute-on-peer request (dynamic)
    PSIRP_PKT_COMPUTE_RESP///< Compute-on-peer response (signed)
} psirp_pkt_type;

/**
 * @brief Interest packet — request for content by name.
 */
typedef struct {
    psirp_name  name;       ///< Content we want
    uint64_t    nonce;      ///< Unique request ID (to dedup)
    uint32_t    lifetime_ms; ///< How long to wait (0 = default 4000ms)
} psirp_interest;

/**
 * @brief Data packet — response with content.
 */
typedef struct {
    psirp_name  name;       ///< Content name
    const uint8_t *data;    ///< Content data (owned by content store)
    size_t      data_len;   ///< Content length
    uint64_t    timestamp;  ///< When published (ns since epoch)
    uint32_t    freshness_ms; ///< How long this data is valid (0 = forever)
} psirp_data;

// ── Packet Serialization ──────────────────────────────────────────────────────

/**
 * @brief Serialize interest packet to buffer.
 * @return Number of bytes written, 0 on error
 */
size_t psirp_interest_serialize(const psirp_interest *pkt, uint8_t *buf, size_t buf_len);

/**
 * @brief Deserialize interest packet from buffer.
 * @return true on success
 */
bool psirp_interest_deserialize(psirp_interest *pkt, const uint8_t *buf, size_t buf_len);

/**
 * @brief Serialize data packet to buffer.
 * @return Number of bytes written, 0 on error
 */
size_t psirp_data_serialize(const psirp_data *pkt, uint8_t *buf, size_t buf_len);

/**
 * @brief Deserialize data packet from buffer.
 * @note Data pointer points into buffer (zero-copy).
 */
bool psirp_data_deserialize(psirp_data *pkt, const uint8_t *buf, size_t buf_len);

// ── Content Store ─────────────────────────────────────────────────────────────

/**
 * @brief Content store entry.
 */
typedef struct {
    psirp_name  name;           ///< Content name
    uint8_t    *data;           ///< Content data (heap-allocated)
    size_t      data_len;       ///< Content length
    uint64_t    timestamp;      ///< When published (ns)
    uint32_t    freshness_ms;   ///< Validity period (0 = forever)
    uint64_t    last_access_ns; ///< For LRU eviction
    size_t      access_count;   ///< For eviction policy
    bool        is_chunk;       ///< True if this is a chunk (part of manifest)
} psirp_cs_entry;

/**
 * @brief Size-limited LRU content store.
 *
 * Entries are heap-allocated (malloc) so eviction actually frees memory.
 * When total bytes exceed max_bytes, least-recently-used entries are evicted.
 */
typedef struct {
    psirp_cs_entry entries[PSIRP_CS_MAX_ENTRIES]; ///< Entry table
    size_t          count;                         ///< Current entries
    size_t          capacity;                      ///< Max entries
    size_t          max_bytes;                     ///< Byte budget (0 = unlimited)
    size_t          bytes_used;                    ///< Current bytes held
} psirp_cs;

/**
 * @brief Initialize content store.
 * @param cs          Content store
 * @param arena_mem   UNUSED placeholder (kept for API compat); pass NULL
 * @param arena_size  Ignored
 */
void psirp_cs_init(psirp_cs *cs, void *arena_mem, size_t arena_size);

/**
 * @brief Set the byte budget for LRU eviction (0 = unlimited).
 */
void psirp_cs_set_budget(psirp_cs *cs, size_t max_bytes);

/**
 * @brief Store content in content store. Evicts LRU if over budget.
 * @return true on success, false if full or duplicate
 */
bool psirp_cs_store(psirp_cs *cs, const psirp_name *name, const uint8_t *data, size_t data_len,
                    uint32_t freshness_ms);

/**
 * @brief Look up content by name. Updates LRU timestamp on hit.
 *        For an unversioned name, returns the newest stored version.
 * @return Pointer to entry, or NULL if not found/expired
 */
const psirp_cs_entry *psirp_cs_lookup(psirp_cs *cs, const psirp_name *name);

/**
 * @brief Remove content by name.
 * @return true if found and removed
 */
bool psirp_cs_remove(psirp_cs *cs, const psirp_name *name);

/**
 * @brief Evict expired entries and, if over budget, LRU entries.
 * @return Number of entries evicted
 */
size_t psirp_cs_gc(psirp_cs *cs);

// ── Chunking ───────────────────────────────────────────────────────────────────

/** @brief Manifest entry: one chunk's name + length. */
typedef struct {
    psirp_name  name;       ///< Chunk name (/orig@ver/chunk/<i>)
    size_t      len;        ///< Chunk length
} psirp_chunk_ref;

/** @brief Chunked-content manifest (stored as content under /orig@ver/chunks). */
typedef struct {
    uint64_t        total_len;          ///< Reassembled length
    size_t          chunk_count;
    psirp_chunk_ref chunks[PSIRP_MAX_DATA / PSIRP_CHUNK_SIZE + 2];
} psirp_manifest;

/**
 * @brief Store large content as chunked manifest + chunks in the CS.
 *        Small content (< PSIRP_CHUNK_SIZE) stored directly.
 * @return true on success
 */
bool psirp_cs_store_chunked(psirp_cs *cs, const psirp_name *name,
                            const uint8_t *data, size_t data_len,
                            uint32_t freshness_ms);

/**
 * @brief Reassemble chunked (or direct) content into out_buf.
 * @param out_buf   Output buffer (must be >= total_len)
 * @param out_cap   Capacity of out_buf
 * @param out_len   Output: actual reassembled length
 * @return true on success
 */
bool psirp_cs_lookup_chunked(psirp_cs *cs, const psirp_name *name,
                             uint8_t *out_buf, size_t out_cap, size_t *out_len);

// ── Forwarder ─────────────────────────────────────────────────────────────────

/** @brief Peer address (UDP endpoint). */
typedef struct {
    uint32_t ip;        ///< IPv4 address (network byte order)
    uint16_t port;      ///< UDP port
} psirp_peer;

/** @brief Forwarder table entry — maps name prefix to peer. */
typedef struct {
    psirp_name prefix;      ///< Name prefix
    psirp_peer peer;        ///< Forward to this peer
    size_t     prefix_len;  ///< Number of components in prefix
} psirp_fib_entry;

/** @brief Forwarding Information Base. */
typedef struct {
    psirp_fib_entry entries[256]; ///< FIB entries
    size_t          count;        ///< Current entries
} psirp_fib;

/**
 * @brief Add forwarding entry.
 */
bool psirp_fib_add(psirp_fib *fib, const char *prefix, uint32_t ip, uint16_t port);

/**
 * @brief Find best matching prefix for a name.
 * @return Peer to forward to, or NULL if no match
 */
const psirp_peer *psirp_fib_lookup(const psirp_fib *fib, const psirp_name *name);

#ifdef __cplusplus
}
#endif

#endif // PSIRP_H
