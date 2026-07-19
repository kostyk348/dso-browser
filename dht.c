/**
 * @file dht.c
 * @brief Kademlia-style DHT implementation (see dht.h).
 */

#include "dht.h"
#include <string.h>
#include <time.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void dht_init(dht_node *dht, uint64_t self_id) {
    if (!dht) return;
    memset(dht, 0, sizeof(*dht));
    dht->self_id = self_id;
}

uint64_t dht_id_from_addr(uint32_t ip, uint16_t port) {
    /* Mix ip and port into a 64-bit id via FNV-ish fold. */
    uint64_t h = 14695981039346656037ULL;
    uint8_t b[6];
    memcpy(b, &ip, 4);
    memcpy(b + 4, &port, 2);
    for (int i = 0; i < 6; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ── Peer management ───────────────────────────────────────────────────────────

bool dht_add_peer(dht_node *dht, uint32_t ip, uint16_t port,
                  const uint8_t *pubkey, bool have_key) {
    if (!dht) return false;
    uint64_t id = dht_id_from_addr(ip, port);

    /* Refresh if known. */
    for (size_t i = 0; i < dht->peer_count; i++) {
        if (dht->peers[i].ip == ip && dht->peers[i].port == port) {
            dht->peers[i].last_seen_ns = now_ns();
            dht->peers[i].alive = true;
            if (have_key && !dht->peers[i].have_key && pubkey) {
                memcpy(dht->peers[i].pubkey, pubkey, 32);
                dht->peers[i].have_key = true;
            }
            return false;
        }
    }

    if (dht->peer_count >= DHT_MAX_PEERS) {
        /* Evict oldest dead peer if any. */
        size_t oldest = 0;
        uint64_t oldest_t = UINT64_MAX;
        bool evicted = false;
        for (size_t i = 0; i < dht->peer_count; i++) {
            if (!dht->peers[i].alive && dht->peers[i].last_seen_ns < oldest_t) {
                oldest_t = dht->peers[i].last_seen_ns;
                oldest = i;
                evicted = true;
            }
        }
        if (!evicted) return false; /* table full of live peers */
        memmove(&dht->peers[oldest], &dht->peers[oldest + 1],
                (dht->peer_count - oldest - 1) * sizeof(dht_peer));
        dht->peer_count--;
    }

    dht_peer *p = &dht->peers[dht->peer_count++];
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->ip = ip;
    p->port = port;
    p->last_seen_ns = now_ns();
    p->alive = true;
    if (have_key && pubkey) {
        memcpy(p->pubkey, pubkey, 32);
        p->have_key = true;
    }
    return true;
}

size_t dht_closest_peers(const dht_node *dht, uint64_t target,
                         dht_peer out[], size_t out_max) {
    if (!dht || !out || out_max == 0) return 0;

    /* Compute distances. */
    typedef struct { uint64_t dist; size_t idx; } pair;
    pair order[DHT_MAX_PEERS];
    size_t n = 0;
    for (size_t i = 0; i < dht->peer_count; i++) {
        if (!dht->peers[i].alive) continue;
        order[n].dist = dht_distance(target, dht->peers[i].id);
        order[n].idx = i;
        n++;
    }

    /* Selection sort by distance (small N, simplicity over speed). */
    for (size_t i = 0; i < n; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++)
            if (order[j].dist < order[best].dist) best = j;
        pair t = order[i]; order[i] = order[best]; order[best] = t;
    }

    size_t m = n < out_max ? n : out_max;
    for (size_t i = 0; i < m; i++)
        out[i] = dht->peers[order[i].idx];
    return m;
}

// ── Content records ───────────────────────────────────────────────────────────

static dht_record *find_record(dht_node *dht, uint64_t content_hash) {
    for (size_t i = 0; i < dht->record_count; i++)
        if (dht->records[i].used && dht->records[i].content_hash == content_hash)
            return &dht->records[i];
    return NULL;
}

void dht_announce(dht_node *dht, uint64_t content_hash, uint32_t ip, uint16_t port) {
    if (!dht) return;
    dht_record *rec = find_record(dht, content_hash);
    if (!rec) {
        if (dht->record_count >= (sizeof(dht->records) / sizeof(dht->records[0]))) return;
        rec = &dht->records[dht->record_count++];
        memset(rec, 0, sizeof(*rec));
        rec->used = true;
        rec->content_hash = content_hash;
    }
    /* Add holder if not present. */
    for (size_t i = 0; i < rec->holder_count; i++)
        if (rec->holder_ip[i] == ip && rec->holder_port[i] == port) return;
    if (rec->holder_count >= DHT_MAX_HOLDERS) return;
    rec->holder_ip[rec->holder_count] = ip;
    rec->holder_port[rec->holder_count] = port;
    rec->holder_count++;
}

void dht_put_record(dht_node *dht, const dht_record *rec) {
    if (!dht || !rec || !rec->used) return;
    dht_record *local = find_record(dht, rec->content_hash);
    if (!local) {
        if (dht->record_count >= (sizeof(dht->records) / sizeof(dht->records[0]))) return;
        local = &dht->records[dht->record_count++];
        memset(local, 0, sizeof(*local));
        local->used = true;
        local->content_hash = rec->content_hash;
    }
    for (size_t i = 0; i < rec->holder_count; i++)
        dht_announce(dht, rec->content_hash, rec->holder_ip[i], rec->holder_port[i]);
}

size_t dht_lookup(dht_node *dht, uint64_t content_hash,
                  uint32_t out_ip[DHT_MAX_HOLDERS], uint16_t out_port[DHT_MAX_HOLDERS]) {
    if (!dht) return 0;
    dht_record *rec = find_record(dht, content_hash);
    if (!rec) return 0;
    size_t m = rec->holder_count < DHT_MAX_HOLDERS ? rec->holder_count : DHT_MAX_HOLDERS;
    for (size_t i = 0; i < m; i++) {
        out_ip[i] = rec->holder_ip[i];
        out_port[i] = rec->holder_port[i];
    }
    return m;
}

const dht_record *dht_get_record(const dht_node *dht, uint64_t content_hash) {
    if (!dht) return NULL;
    return find_record((dht_node *)dht, content_hash);
}
