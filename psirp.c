/**
 * @file psirp.c
 * @brief PSIRP implementation — content naming, packets, content store.
 */

#include "psirp.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h>

// ── FNV-1a Hash ───────────────────────────────────────────────────────────────

static uint64_t fnv1a_hash(const char *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// ── Content Name ──────────────────────────────────────────────────────────────

bool psirp_name_init(psirp_name *name, const char *path) {
    if (!name || !path) return false;
    
    memset(name, 0, sizeof(*name));
    
    // Skip leading slash
    if (path[0] == '/') path++;
    
    // Parse components
    const char *p = path;
    while (*p && name->count < PSIRP_MAX_COMPONENTS) {
        const char *start = p;
        while (*p && *p != '/') p++;
        
        size_t comp_len = (size_t)(p - start);
        if (comp_len == 0) { p++; continue; } // skip empty (double slash)
        if (comp_len >= PSIRP_MAX_COMPONENT) return false; // component too long
        
        // Allocate component string
        char *comp = (char *)malloc(comp_len + 1);
        if (!comp) return false;
        memcpy(comp, start, comp_len);
        comp[comp_len] = '\0';
        
        name->components[name->count++] = comp;
        
        if (*p == '/') p++;
    }
    
    // Compute hash — include leading slash for consistency
    char hash_buf[PSIRP_MAX_NAME];
    hash_buf[0] = '/';
    size_t hash_len = strlen(path) + 1;
    memcpy(hash_buf + 1, path, hash_len - 1);
    name->hash = fnv1a_hash(hash_buf, hash_len);
    
    return true;
}

bool psirp_name_init_components(psirp_name *name, const char **components, size_t count) {
    if (!name || count > PSIRP_MAX_COMPONENTS) return false;
    
    memset(name, 0, sizeof(*name));
    
    // Build path string for hashing
    char path[PSIRP_MAX_NAME] = {0};
    size_t pos = 0;
    
    for (size_t i = 0; i < count; i++) {
        size_t comp_len = strlen(components[i]);
        if (comp_len >= PSIRP_MAX_COMPONENT) return false;
        
        path[pos++] = '/';
        memcpy(path + pos, components[i], comp_len);
        pos += comp_len;
        
        // Store component
        char *comp = (char *)malloc(comp_len + 1);
        if (!comp) return false;
        memcpy(comp, components[i], comp_len);
        comp[comp_len] = '\0';
        name->components[name->count++] = comp;
    }
    
    name->hash = fnv1a_hash(path, pos);
    return true;
}

bool psirp_name_equal(const psirp_name *a, const psirp_name *b) {
    if (!a || !b) return false;
    if (a->count != b->count) return false;
    if (a->hash != b->hash) return false; // fast reject
    
    for (size_t i = 0; i < a->count; i++) {
        if (strcmp(a->components[i], b->components[i]) != 0) return false;
    }
    return true;
}

bool psirp_name_is_prefix(const psirp_name *prefix, const psirp_name *name) {
    if (!prefix || !name) return false;
    if (prefix->count > name->count) return false;
    
    for (size_t i = 0; i < prefix->count; i++) {
        if (strcmp(prefix->components[i], name->components[i]) != 0) return false;
    }
    return true;
}

size_t psirp_name_to_string(const psirp_name *name, char *buf, size_t buf_len) {
    if (!name || !buf || buf_len == 0) return 0;
    
    size_t pos = 0;
    for (size_t i = 0; i < name->count && pos < buf_len - 1; i++) {
        buf[pos++] = '/';
        size_t comp_len = strlen(name->components[i]);
        if (pos + comp_len >= buf_len - 1) break;
        memcpy(buf + pos, name->components[i], comp_len);
        pos += comp_len;
    }
    buf[pos] = '\0';
    return pos;
}

// ── Packet Serialization ──────────────────────────────────────────────────────

/**
 * Wire format:
 *
 * Interest:
 *   [1B type][8B nonce][4B lifetime_ms][1B num_components]
 *   [for each component: 1B len][component bytes...]
 *
 * Data:
 *   [1B type][8B timestamp][4B freshness_ms][4B data_len]
 *   [1B num_components][for each: 1B len][component bytes...]
 *   [data bytes...]
 */

size_t psirp_interest_serialize(const psirp_interest *pkt, uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf) return 0;
    
    size_t pos = 0;
    
    // Type
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = PSIRP_PKT_INTEREST;
    
    // Nonce (network byte order)
    if (pos + 8 > buf_len) return 0;
    uint64_t nonce = htobe64(pkt->nonce);
    memcpy(buf + pos, &nonce, 8);
    pos += 8;
    
    // Lifetime
    if (pos + 4 > buf_len) return 0;
    uint32_t lifetime = htonl(pkt->lifetime_ms);
    memcpy(buf + pos, &lifetime, 4);
    pos += 4;
    
    // Components
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = (uint8_t)pkt->name.count;
    
    for (size_t i = 0; i < pkt->name.count; i++) {
        size_t comp_len = strlen(pkt->name.components[i]);
        if (comp_len > 255) return 0;
        if (pos + 1 + comp_len > buf_len) return 0;
        buf[pos++] = (uint8_t)comp_len;
        memcpy(buf + pos, pkt->name.components[i], comp_len);
        pos += comp_len;
    }
    
    return pos;
}

bool psirp_interest_deserialize(psirp_interest *pkt, const uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf || buf_len < 14) return false; // minimum: type(1) + nonce(8) + lifetime(4) + count(1)
    
    size_t pos = 0;
    
    // Type check
    if (buf[pos++] != PSIRP_PKT_INTEREST) return false;
    
    // Nonce
    uint64_t nonce;
    memcpy(&nonce, buf + pos, 8);
    pkt->nonce = be64toh(nonce);
    pos += 8;
    
    // Lifetime
    uint32_t lifetime;
    memcpy(&lifetime, buf + pos, 4);
    pkt->lifetime_ms = ntohl(lifetime);
    pos += 4;
    
    // Components
    if (pos + 1 > buf_len) return false;
    pkt->name.count = buf[pos++];
    if (pkt->name.count > PSIRP_MAX_COMPONENTS) return false;
    
    // Build hash buffer as we parse components
    char hash_buf[PSIRP_MAX_NAME];
    size_t hash_pos = 0;
    
    for (size_t i = 0; i < pkt->name.count; i++) {
        if (pos + 1 > buf_len) return false;
        uint8_t comp_len = buf[pos++];
        if (pos + comp_len > buf_len) return false;
        
        char *comp = (char *)malloc(comp_len + 1);
        if (!comp) return false;
        memcpy(comp, buf + pos, comp_len);
        comp[comp_len] = '\0';
        pkt->name.components[i] = comp;
        pos += comp_len;
        
        // Build path for hashing
        if (hash_pos + 1 + comp_len < PSIRP_MAX_NAME) {
            hash_buf[hash_pos++] = '/';
            memcpy(hash_buf + hash_pos, comp, comp_len);
            hash_pos += comp_len;
        }
    }
    pkt->name.hash = fnv1a_hash(hash_buf, hash_pos);
    
    return true;
}

size_t psirp_data_serialize(const psirp_data *pkt, uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf) return 0;
    
    size_t pos = 0;
    
    // Type
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = PSIRP_PKT_DATA;
    
    // Timestamp
    if (pos + 8 > buf_len) return 0;
    uint64_t ts = htobe64(pkt->timestamp);
    memcpy(buf + pos, &ts, 8);
    pos += 8;
    
    // Freshness
    if (pos + 4 > buf_len) return 0;
    uint32_t fresh = htonl(pkt->freshness_ms);
    memcpy(buf + pos, &fresh, 4);
    pos += 4;
    
    // Data length
    if (pos + 4 > buf_len) return 0;
    uint32_t dlen = htonl((uint32_t)pkt->data_len);
    memcpy(buf + pos, &dlen, 4);
    pos += 4;
    
    // Components
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = (uint8_t)pkt->name.count;
    
    for (size_t i = 0; i < pkt->name.count; i++) {
        size_t comp_len = strlen(pkt->name.components[i]);
        if (comp_len > 255) return 0;
        if (pos + 1 + comp_len > buf_len) return 0;
        buf[pos++] = (uint8_t)comp_len;
        memcpy(buf + pos, pkt->name.components[i], comp_len);
        pos += comp_len;
    }
    
    // Data
    if (pos + pkt->data_len > buf_len) return 0;
    memcpy(buf + pos, pkt->data, pkt->data_len);
    pos += pkt->data_len;
    
    return pos;
}

bool psirp_data_deserialize(psirp_data *pkt, const uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf || buf_len < 18) return false; // minimum header
    
    size_t pos = 0;
    
    // Type check
    if (buf[pos++] != PSIRP_PKT_DATA) return false;
    
    // Timestamp
    uint64_t ts;
    memcpy(&ts, buf + pos, 8);
    pkt->timestamp = be64toh(ts);
    pos += 8;
    
    // Freshness
    uint32_t fresh;
    memcpy(&fresh, buf + pos, 4);
    pkt->freshness_ms = ntohl(fresh);
    pos += 4;
    
    // Data length
    uint32_t dlen;
    memcpy(&dlen, buf + pos, 4);
    pkt->data_len = ntohl(dlen);
    pos += 4;
    
    // Components
    if (pos + 1 > buf_len) return false;
    pkt->name.count = buf[pos++];
    if (pkt->name.count > PSIRP_MAX_COMPONENTS) return false;
    
    // Build hash buffer as we parse components
    char hash_buf[PSIRP_MAX_NAME];
    size_t hash_pos = 0;
    
    for (size_t i = 0; i < pkt->name.count; i++) {
        if (pos + 1 > buf_len) return false;
        uint8_t comp_len = buf[pos++];
        if (pos + comp_len > buf_len) return false;
        
        char *comp = (char *)malloc(comp_len + 1);
        if (!comp) return false;
        memcpy(comp, buf + pos, comp_len);
        comp[comp_len] = '\0';
        pkt->name.components[i] = comp;
        pos += comp_len;
        
        // Build path for hashing
        if (hash_pos + 1 + comp_len < PSIRP_MAX_NAME) {
            hash_buf[hash_pos++] = '/';
            memcpy(hash_buf + hash_pos, comp, comp_len);
            hash_pos += comp_len;
        }
    }
    pkt->name.hash = fnv1a_hash(hash_buf, hash_pos);
    
    // Data pointer (zero-copy: points into buffer)
    if (pos + pkt->data_len > buf_len) return false;
    pkt->data = buf + pos;
    
    return true;
}

// ── Content Store ─────────────────────────────────────────────────────────────

void psirp_cs_init(psirp_cs *cs, void *arena_mem, size_t arena_size) {
    if (!cs) return;
    
    memset(cs, 0, sizeof(*cs));
    cs->capacity = PSIRP_CS_MAX_ENTRIES;
    cs->arena_mem = (uint8_t *)arena_mem;
    cs->arena_size = arena_size;
    cs->arena_offset = 0;
}

static uint8_t *cs_arena_alloc(psirp_cs *cs, size_t size) {
    // Align to 8 bytes
    size_t aligned = (size + 7) & ~7;
    
    if (cs->arena_offset + aligned > cs->arena_size) return NULL;
    
    uint8_t *ptr = cs->arena_mem + cs->arena_offset;
    cs->arena_offset += aligned;
    return ptr;
}

bool psirp_cs_store(psirp_cs *cs, const psirp_name *name, const uint8_t *data, size_t data_len,
                    uint32_t freshness_ms) {
    if (!cs || !name || !data || data_len == 0) return false;
    if (data_len > PSIRP_MAX_DATA) return false;
    
    // Check for duplicate
    for (size_t i = 0; i < cs->count; i++) {
        if (psirp_name_equal(&cs->entries[i].name, name)) {
            // Update existing
            cs->entries[i].data_len = data_len;
            cs->entries[i].timestamp = (uint64_t)time(NULL) * 1000000000ULL;
            cs->entries[i].freshness_ms = freshness_ms;
            return true;
        }
    }
    
    // Check capacity
    if (cs->count >= cs->capacity) return false;
    
    // Allocate in arena
    uint8_t *data_buf = cs_arena_alloc(cs, data_len);
    if (!data_buf) return false;
    
    // Store name components
    const char *components[PSIRP_MAX_COMPONENTS];
    for (size_t i = 0; i < name->count; i++) {
        components[i] = name->components[i];
    }
    
    psirp_cs_entry *entry = &cs->entries[cs->count];
    memset(entry, 0, sizeof(*entry));
    
    // Reconstruct name in entry
    psirp_name_init_components(&entry->name, components, name->count);
    
    // Copy data
    memcpy(data_buf, data, data_len);
    entry->data = data_buf;
    entry->data_len = data_len;
    entry->timestamp = (uint64_t)time(NULL) * 1000000000ULL;
    entry->freshness_ms = freshness_ms;
    entry->access_count = 0;
    
    cs->count++;
    return true;
}

const psirp_cs_entry *psirp_cs_lookup(psirp_cs *cs, const psirp_name *name) {
    if (!cs || !name) return NULL;
    
    uint64_t now = (uint64_t)time(NULL) * 1000000000ULL;
    
    for (size_t i = 0; i < cs->count; i++) {
        if (psirp_name_equal(&cs->entries[i].name, name)) {
            // Check freshness
            if (cs->entries[i].freshness_ms > 0) {
                uint64_t age_ms = (now - cs->entries[i].timestamp) / 1000000;
                if (age_ms > cs->entries[i].freshness_ms) {
                    continue; // expired
                }
            }
            
            cs->entries[i].access_count++;
            return &cs->entries[i];
        }
    }
    
    return NULL;
}

bool psirp_cs_remove(psirp_cs *cs, const psirp_name *name) {
    if (!cs || !name) return false;
    
    for (size_t i = 0; i < cs->count; i++) {
        if (psirp_name_equal(&cs->entries[i].name, name)) {
            // Free name components
            for (size_t j = 0; j < cs->entries[i].name.count; j++) {
                free((void *)cs->entries[i].name.components[j]);
            }
            
            // Move last entry here (swap remove)
            if (i < cs->count - 1) {
                cs->entries[i] = cs->entries[cs->count - 1];
            }
            cs->count--;
            return true;
        }
    }
    
    return false;
}

size_t psirp_cs_gc(psirp_cs *cs) {
    if (!cs) return 0;
    
    uint64_t now = (uint64_t)time(NULL) * 1000000000ULL;
    size_t evicted = 0;
    
    for (size_t i = cs->count; i > 0; i--) {
        size_t idx = i - 1;
        if (cs->entries[idx].freshness_ms > 0) {
            uint64_t age_ms = (now - cs->entries[idx].timestamp) / 1000000;
            if (age_ms > cs->entries[idx].freshness_ms) {
                // Free name components
                for (size_t j = 0; j < cs->entries[idx].name.count; j++) {
                    free((void *)cs->entries[idx].name.components[j]);
                }
                
                // Swap remove
                if (idx < cs->count - 1) {
                    cs->entries[idx] = cs->entries[cs->count - 1];
                }
                cs->count--;
                evicted++;
            }
        }
    }
    
    return evicted;
}

// ── Forwarding Information Base ───────────────────────────────────────────────

bool psirp_fib_add(psirp_fib *fib, const char *prefix, uint32_t ip, uint16_t port) {
    if (!fib || !prefix || fib->count >= 256) return false;
    
    psirp_fib_entry *entry = &fib->entries[fib->count];
    memset(entry, 0, sizeof(*entry));
    
    if (!psirp_name_init(&entry->prefix, prefix)) return false;
    
    entry->peer.ip = ip;
    entry->peer.port = htons(port); // Store in network byte order
    entry->prefix_len = entry->prefix.count;
    
    fib->count++;
    return true;
}

const psirp_peer *psirp_fib_lookup(const psirp_fib *fib, const psirp_name *name) {
    if (!fib || !name) return NULL;
    
    const psirp_peer *best = NULL;
    size_t best_len = 0;
    
    for (size_t i = 0; i < fib->count; i++) {
        if (psirp_name_is_prefix(&fib->entries[i].prefix, name)) {
            if (fib->entries[i].prefix_len > best_len) {
                best_len = fib->entries[i].prefix_len;
                best = &fib->entries[i].peer;
            }
        }
    }
    
    return best;
}
