/**
 * @file psirp.c
 * @brief PSIRP implementation — content naming, packets, content store.
 *
 * Features:
 *  - Hierarchical content names with optional @version (pub/sub "latest")
 *  - Size-limited LRU content store (heap-backed, evicts on byte budget)
 *  - Content chunking for large objects (manifest + 64KB chunks)
 */

#include "psirp.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

// ── Time helper ───────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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

/** @brief Parse a single component, splitting off a trailing "@version". */
static bool parse_component(const char *src, size_t len, char *out, size_t out_cap,
                            uint64_t *version, bool *has_version) {
    if (len >= out_cap) return false;
    memcpy(out, src, len);
    out[len] = '\0';
    *version = 0;
    *has_version = false;

    /* Look for '@' that introduces a version number. */
    const char *at = NULL;
    for (size_t i = 0; i < len; i++)
        if (src[i] == '@') { at = src + i; break; }

    if (at) {
        /* Ensure the part after '@' is a pure decimal number. */
        const char *v = at + 1;
        if (*v == '\0') return false;
        uint64_t ver = 0;
        for (const char *p = v; *p; p++) {
            if (*p < '0' || *p > '9') return false;
            ver = ver * 10 + (uint64_t)(*p - '0');
        }
        /* Split: component text stops before '@'. */
        size_t base_len = (size_t)(at - src);
        if (base_len >= out_cap) return false;
        memcpy(out, src, base_len);
        out[base_len] = '\0';
        *version = ver;
        *has_version = true;
    }
    return true;
}

bool psirp_name_init(psirp_name *name, const char *path) {
    if (!name || !path) return false;
    memset(name, 0, sizeof(*name));

    if (path[0] == '/') path++;

    const char *p = path;
    char comp_buf[PSIRP_MAX_COMPONENT];
    while (*p && name->count < PSIRP_MAX_COMPONENTS) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t comp_len = (size_t)(p - start);
        if (comp_len == 0) { p++; continue; }
        if (comp_len >= PSIRP_MAX_COMPONENT) return false;

        uint64_t ver;
        bool has_ver;
        if (!parse_component(start, comp_len, comp_buf, sizeof(comp_buf), &ver, &has_ver))
            return false;

        char *comp = (char *)malloc(strlen(comp_buf) + 1);
        if (!comp) return false;
        strcpy(comp, comp_buf);
        name->components[name->count++] = comp;
        if (has_ver) { name->version = ver; name->has_version = true; }

        if (*p == '/') p++;
    }

    /* Hash over the full path text (version included, for stable identity). */
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

    char path[PSIRP_MAX_NAME] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        size_t comp_len = strlen(components[i]);
        if (comp_len >= PSIRP_MAX_COMPONENT) return false;
        path[pos++] = '/';
        memcpy(path + pos, components[i], comp_len);
        pos += comp_len;

        char comp_buf[PSIRP_MAX_COMPONENT];
        uint64_t ver;
        bool has_ver;
        if (!parse_component(components[i], comp_len, comp_buf, sizeof(comp_buf), &ver, &has_ver))
            return false;
        char *comp = (char *)malloc(strlen(comp_buf) + 1);
        if (!comp) return false;
        strcpy(comp, comp_buf);
        name->components[name->count++] = comp;
        if (has_ver) { name->version = ver; name->has_version = true; }
    }
    name->hash = fnv1a_hash(path, pos);
    return true;
}

/** @brief Compare base components only (ignore version). */
static bool name_base_equal(const psirp_name *a, const psirp_name *b) {
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->components[i], b->components[i]) != 0) return false;
    return true;
}

bool psirp_name_equal(const psirp_name *a, const psirp_name *b) {
    if (!a || !b) return false;
    if (a->count != b->count) return false;
    if (a->hash != b->hash) return false;
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->components[i], b->components[i]) != 0) return false;
    return true;
}

bool psirp_name_is_prefix(const psirp_name *prefix, const psirp_name *name) {
    if (!prefix || !name) return false;
    if (prefix->count > name->count) return false;
    for (size_t i = 0; i < prefix->count; i++)
        if (strcmp(prefix->components[i], name->components[i]) != 0) return false;
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
        /* append @version if present and this is the last component */
        if (name->has_version && i + 1 == name->count && pos + 1 < buf_len - 1) {
            char vbuf[24];
            int n = snprintf(vbuf, sizeof(vbuf), "@%llu", (unsigned long long)name->version);
            if ((size_t)n < sizeof(vbuf)) {
                memcpy(buf + pos, vbuf, (size_t)n);
                pos += (size_t)n;
            }
        }
    }
    buf[pos] = '\0';
    return pos;
}

// ── Packet Serialization ──────────────────────────────────────────────────────

size_t psirp_interest_serialize(const psirp_interest *pkt, uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf) return 0;
    size_t pos = 0;
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = PSIRP_PKT_INTEREST;
    if (pos + 8 > buf_len) return 0;
    uint64_t nonce = htobe64(pkt->nonce);
    memcpy(buf + pos, &nonce, 8);
    pos += 8;
    if (pos + 4 > buf_len) return 0;
    uint32_t lifetime = htonl(pkt->lifetime_ms);
    memcpy(buf + pos, &lifetime, 4);
    pos += 4;
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
    if (!pkt || !buf || buf_len < 14) return false;
    size_t pos = 0;
    if (buf[pos++] != PSIRP_PKT_INTEREST) return false;
    uint64_t nonce;
    memcpy(&nonce, buf + pos, 8);
    pkt->nonce = be64toh(nonce);
    pos += 8;
    uint32_t lifetime;
    memcpy(&lifetime, buf + pos, 4);
    pkt->lifetime_ms = ntohl(lifetime);
    pos += 4;
    if (pos + 1 > buf_len) return false;
    pkt->name.count = buf[pos++];
    if (pkt->name.count > PSIRP_MAX_COMPONENTS) return false;
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
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = PSIRP_PKT_DATA;
    if (pos + 8 > buf_len) return 0;
    uint64_t ts = htobe64(pkt->timestamp);
    memcpy(buf + pos, &ts, 8);
    pos += 8;
    if (pos + 4 > buf_len) return 0;
    uint32_t fresh = htonl(pkt->freshness_ms);
    memcpy(buf + pos, &fresh, 4);
    pos += 4;
    if (pos + 4 > buf_len) return 0;
    uint32_t dlen = htonl((uint32_t)pkt->data_len);
    memcpy(buf + pos, &dlen, 4);
    pos += 4;
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
    if (pos + pkt->data_len > buf_len) return 0;
    memcpy(buf + pos, pkt->data, pkt->data_len);
    pos += pkt->data_len;
    return pos;
}

bool psirp_data_deserialize(psirp_data *pkt, const uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf || buf_len < 18) return false;
    size_t pos = 0;
    if (buf[pos++] != PSIRP_PKT_DATA) return false;
    uint64_t ts;
    memcpy(&ts, buf + pos, 8);
    pkt->timestamp = be64toh(ts);
    pos += 8;
    uint32_t fresh;
    memcpy(&fresh, buf + pos, 4);
    pkt->freshness_ms = ntohl(fresh);
    pos += 4;
    uint32_t dlen;
    memcpy(&dlen, buf + pos, 4);
    pkt->data_len = ntohl(dlen);
    pos += 4;
    if (pos + 1 > buf_len) return false;
    pkt->name.count = buf[pos++];
    if (pkt->name.count > PSIRP_MAX_COMPONENTS) return false;
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
        if (hash_pos + 1 + comp_len < PSIRP_MAX_NAME) {
            hash_buf[hash_pos++] = '/';
            memcpy(hash_buf + hash_pos, comp, comp_len);
            hash_pos += comp_len;
        }
    }
    pkt->name.hash = fnv1a_hash(hash_buf, hash_pos);
    if (pos + pkt->data_len > buf_len) return false;
    pkt->data = buf + pos;
    return true;
}

// ── Content Store (LRU, byte-budgeted) ─────────────────────────────────────────

void psirp_cs_init(psirp_cs *cs, void *arena_mem, size_t arena_size) {
    (void)arena_mem; (void)arena_size;
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->capacity = PSIRP_CS_MAX_ENTRIES;
    cs->max_bytes = 0; /* unlimited until set */
}

void psirp_cs_set_budget(psirp_cs *cs, size_t max_bytes) {
    if (cs) cs->max_bytes = max_bytes;
}

static void free_entry(psirp_cs_entry *e) {
    for (size_t j = 0; j < e->name.count; j++)
        free((void *)e->name.components[j]);
    free(e->data);
    memset(e, 0, sizeof(*e));
}

static bool entry_expired(const psirp_cs_entry *e, uint64_t now) {
    if (e->freshness_ms == 0) return false;
    uint64_t age_ms = (now - e->timestamp) / 1000000ULL;
    return age_ms > e->freshness_ms;
}

bool psirp_cs_store(psirp_cs *cs, const psirp_name *name, const uint8_t *data, size_t data_len,
                    uint32_t freshness_ms) {
    if (!cs || !name || !data || data_len == 0) return false;
    if (data_len > PSIRP_MAX_DATA) return false;

    uint64_t now = now_ns();

    /* Duplicate? update in place. */
    for (size_t i = 0; i < cs->count; i++) {
        if (psirp_name_equal(&cs->entries[i].name, name)) {
            uint8_t *nb = (uint8_t *)malloc(data_len);
            if (!nb) return false;
            memcpy(nb, data, data_len);
            free(cs->entries[i].data);
            cs->bytes_used -= cs->entries[i].data_len;
            cs->entries[i].data = nb;
            cs->entries[i].data_len = data_len;
            cs->entries[i].timestamp = now;
            cs->entries[i].freshness_ms = freshness_ms;
            cs->entries[i].last_access_ns = now;
            cs->bytes_used += data_len;
            return true;
        }
    }

    if (cs->count >= cs->capacity) return false;

    uint8_t *nb = (uint8_t *)malloc(data_len);
    if (!nb) return false;
    memcpy(nb, data, data_len);

    psirp_cs_entry *e = &cs->entries[cs->count];
    memset(e, 0, sizeof(*e));
    /* copy name by re-init from components */
    char *comps[PSIRP_MAX_COMPONENTS];
    for (size_t i = 0; i < name->count; i++) {
        comps[i] = (char *)malloc(strlen(name->components[i]) + 1);
        strcpy(comps[i], name->components[i]);
    }
    psirp_name_init_components(&e->name, (const char **)comps, name->count);
    for (size_t i = 0; i < name->count; i++) free(comps[i]); /* name kept its own copy */
    e->name.version = name->version;
    e->name.has_version = name->has_version;

    e->data = nb;
    e->data_len = data_len;
    e->timestamp = now;
    e->freshness_ms = freshness_ms;
    e->last_access_ns = now;
    e->access_count = 0;
    e->is_chunk = false;

    cs->bytes_used += data_len;
    cs->count++;

    /* Enforce byte budget by evicting LRU. */
    if (cs->max_bytes > 0) {
        while (cs->bytes_used > cs->max_bytes && cs->count > 0) {
            size_t lru = 0;
            for (size_t i = 1; i < cs->count; i++)
                if (cs->entries[i].last_access_ns < cs->entries[lru].last_access_ns) lru = i;
            cs->bytes_used -= cs->entries[lru].data_len;
            free_entry(&cs->entries[lru]);
            /* swap-remove */
            if (lru < cs->count - 1)
                cs->entries[lru] = cs->entries[cs->count - 1];
            cs->count--;
        }
    }
    return true;
}

const psirp_cs_entry *psirp_cs_lookup(psirp_cs *cs, const psirp_name *name) {
    if (!cs || !name) return NULL;
    uint64_t now = now_ns();

    /* Versioned exact match first. */
    if (name->has_version) {
        for (size_t i = 0; i < cs->count; i++) {
            if (entry_expired(&cs->entries[i], now)) continue;
            if (psirp_name_equal(&cs->entries[i].name, name)) {
                cs->entries[i].last_access_ns = now;
                cs->entries[i].access_count++;
                return &cs->entries[i];
            }
        }
        return NULL;
    }

    /* Unversioned: return the NEWEST version with matching base components. */
    const psirp_cs_entry *best = NULL;
    for (size_t i = 0; i < cs->count; i++) {
        if (entry_expired(&cs->entries[i], now)) continue;
        if (name_base_equal(&cs->entries[i].name, name)) {
            if (!best || cs->entries[i].name.version > best->name.version) {
                if (cs->entries[i].name.version == 0 && best) {
                    /* prefer explicit-versioned over version-0 if both exist? keep newest */
                }
                best = &cs->entries[i];
            }
        }
    }
    if (best) {
        /* const cast only to update LRU bookkeeping */
        ((psirp_cs_entry *)best)->last_access_ns = now;
        ((psirp_cs_entry *)best)->access_count++;
    }
    return best;
}

bool psirp_cs_remove(psirp_cs *cs, const psirp_name *name) {
    if (!cs || !name) return false;
    for (size_t i = 0; i < cs->count; i++) {
        if (psirp_name_equal(&cs->entries[i].name, name)) {
            cs->bytes_used -= cs->entries[i].data_len;
            free_entry(&cs->entries[i]);
            if (i < cs->count - 1)
                cs->entries[i] = cs->entries[cs->count - 1];
            cs->count--;
            return true;
        }
    }
    return false;
}

size_t psirp_cs_gc(psirp_cs *cs) {
    if (!cs) return 0;
    uint64_t now = now_ns();
    size_t evicted = 0;
    for (size_t i = cs->count; i > 0; i--) {
        size_t idx = i - 1;
        if (entry_expired(&cs->entries[idx], now)) {
            cs->bytes_used -= cs->entries[idx].data_len;
            free_entry(&cs->entries[idx]);
            if (idx < cs->count - 1)
                cs->entries[idx] = cs->entries[cs->count - 1];
            cs->count--;
            evicted++;
        }
    }
    /* Also enforce byte budget if set. */
    if (cs->max_bytes > 0) {
        while (cs->bytes_used > cs->max_bytes && cs->count > 0) {
            size_t lru = 0;
            for (size_t i = 1; i < cs->count; i++)
                if (cs->entries[i].last_access_ns < cs->entries[lru].last_access_ns) lru = i;
            cs->bytes_used -= cs->entries[lru].data_len;
            free_entry(&cs->entries[lru]);
            if (lru < cs->count - 1)
                cs->entries[lru] = cs->entries[cs->count - 1];
            cs->count--;
            evicted++;
        }
    }
    return evicted;
}

// ── Chunking ───────────────────────────────────────────────────────────────────

bool psirp_cs_store_chunked(psirp_cs *cs, const psirp_name *name,
                            const uint8_t *data, size_t data_len,
                            uint32_t freshness_ms) {
    if (!cs || !name || !data) return false;

    /* Small content: store directly. */
    if (data_len <= PSIRP_CHUNK_SIZE) {
        return psirp_cs_store(cs, name, data, data_len, freshness_ms);
    }

    psirp_manifest m;
    memset(&m, 0, sizeof(m));
    m.total_len = data_len;

    size_t off = 0, ci = 0;
    while (off < data_len && ci < (sizeof(m.chunks) / sizeof(m.chunks[0]))) {
        size_t chunk_len = data_len - off;
        if (chunk_len > PSIRP_CHUNK_SIZE) chunk_len = PSIRP_CHUNK_SIZE;

        /* Chunk name: /orig@ver/chunk/<i> */
        char cname[PSIRP_MAX_NAME];
        char base[PSIRP_MAX_NAME];
        psirp_name_to_string(name, base, sizeof(base));
        snprintf(cname, sizeof(cname), "%s/chunk/%zu", base, ci);

        psirp_name cn;
        if (!psirp_name_init(&cn, cname)) return false;
        if (!psirp_cs_store(cs, &cn, data + off, chunk_len, freshness_ms))
            return false;

        m.chunks[ci].name = cn; /* take ownership of components */
        m.chunks[ci].len = chunk_len;
        m.chunk_count++;
        off += chunk_len;
        ci++;
    }

    /* Store manifest under /orig@ver/chunks */
    char mname[PSIRP_MAX_NAME];
    char base[PSIRP_MAX_NAME];
    psirp_name_to_string(name, base, sizeof(base));
    snprintf(mname, sizeof(mname), "%s/chunks", base);
    psirp_name mn;
    if (!psirp_name_init(&mn, mname)) return false;

    uint8_t mbuf[sizeof(psirp_manifest)];
    memcpy(mbuf, &m, sizeof(m));
    bool ok = psirp_cs_store(cs, &mn, mbuf, sizeof(m), freshness_ms);
    /* mn and chunk names own their component strings; free them if store failed */
    if (!ok) {
        for (size_t i = 0; i < m.chunk_count; i++)
            for (size_t j = 0; j < m.chunks[i].name.count; j++)
                free((void *)m.chunks[i].name.components[j]);
        for (size_t j = 0; j < mn.count; j++) free((void *)mn.components[j]);
    }
    (void)mn;
    return ok;
}

bool psirp_cs_lookup_chunked(psirp_cs *cs, const psirp_name *name,
                             uint8_t *out_buf, size_t out_cap, size_t *out_len) {
    if (!cs || !name || !out_buf || !out_len) return false;

    /* Manifest name */
    char mname[PSIRP_MAX_NAME];
    char base[PSIRP_MAX_NAME];
    psirp_name_to_string(name, base, sizeof(base));
    snprintf(mname, sizeof(mname), "%s/chunks", base);
    psirp_name mn;
    if (!psirp_name_init(&mn, mname)) return false;

    const psirp_cs_entry *me = psirp_cs_lookup(cs, &mn);
    if (!me) {
        /* Maybe it was stored unchunked (small). */
        const psirp_cs_entry *e = psirp_cs_lookup(cs, name);
        if (e && e->data_len <= out_cap) {
            memcpy(out_buf, e->data, e->data_len);
            *out_len = e->data_len;
            return true;
        }
        return false;
    }

    psirp_manifest m;
    if (me->data_len != sizeof(m)) return false;
    memcpy(&m, me->data, sizeof(m));

    if (m.total_len > out_cap) return false;
    size_t off = 0;
    for (size_t i = 0; i < m.chunk_count; i++) {
        const psirp_cs_entry *ce = psirp_cs_lookup(cs, &m.chunks[i].name);
        if (!ce) return false;
        memcpy(out_buf + off, ce->data, ce->data_len);
        off += ce->data_len;
    }
    *out_len = off;
    return off == m.total_len;
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
