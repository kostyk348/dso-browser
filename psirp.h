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
#define PSIRP_CS_MAX_ENTRIES  1024   ///< Max entries in content store
#define PSIRP_NONCE_RANDOM    1      ///< Use random nonce

// ── Content Name ──────────────────────────────────────────────────────────────

/**
 * @brief Hierarchical content name.
 *
 * Example: /github/user/repo/README.md
 * Components: ["github", "user", "repo", "README.md"]
 */
typedef struct {
    const char *components[PSIRP_MAX_COMPONENTS]; ///< Path components
    size_t      count;                             ///< Number of components
    uint64_t    hash;                              ///< FNV-1a hash for fast lookup
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
    PSIRP_PKT_PUBLISH,    ///< Announce new content
    PSIRP_PKT_CANCEL      ///< Cancel pending interest
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
    uint8_t    *data;           ///< Content data (in arena)
    size_t      data_len;       ///< Content length
    uint64_t    timestamp;      ///< When published
    uint32_t    freshness_ms;   ///< Validity period (0 = forever)
    size_t      access_count;   ///< For eviction policy
} psirp_cs_entry;

/**
 * @brief Arena-backed content store.
 */
typedef struct {
    psirp_cs_entry entries[PSIRP_CS_MAX_ENTRIES]; ///< Entry table
    size_t          count;                         ///< Current entries
    size_t          capacity;                      ///< Max entries
    uint8_t        *arena_mem;                     ///< Arena backing memory
    size_t          arena_size;                    ///< Arena size
    size_t          arena_offset;                  ///< Current arena offset
} psirp_cs;

/**
 * @brief Initialize content store.
 * @param cs        Content store
 * @param arena_mem Pre-allocated memory for content
 * @param arena_size Size of arena memory
 */
void psirp_cs_init(psirp_cs *cs, void *arena_mem, size_t arena_size);

/**
 * @brief Store content in content store.
 * @return true on success, false if full or duplicate
 */
bool psirp_cs_store(psirp_cs *cs, const psirp_name *name, const uint8_t *data, size_t data_len,
                    uint32_t freshness_ms);

/**
 * @brief Look up content by name.
 * @return Pointer to entry, or NULL if not found/expired
 */
const psirp_cs_entry *psirp_cs_lookup(psirp_cs *cs, const psirp_name *name);

/**
 * @brief Remove content by name.
 * @return true if found and removed
 */
bool psirp_cs_remove(psirp_cs *cs, const psirp_name *name);

/**
 * @brief Evict expired entries.
 * @return Number of entries evicted
 */
size_t psirp_cs_gc(psirp_cs *cs);

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
