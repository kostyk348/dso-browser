/**
 * @file pubsub.c
 * @brief Publish/Subscribe implementation (see pubsub.h).
 */

#include "pubsub.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>

void pubsub_init(pubsub_ctx *ctx, dht_node *dht, uint32_t self_ip, uint16_t self_port) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->dht = dht;
    ctx->self_ip = self_ip;
    ctx->self_port = self_port;
}

static pubsub_topic *find_topic(pubsub_ctx *ctx, const psirp_name *base) {
    for (size_t i = 0; i < ctx->topic_count; i++)
        if (psirp_name_equal(&ctx->topics[i].base, base)) return &ctx->topics[i];
    return NULL;
}

bool pubsub_publish(pubsub_ctx *ctx, const psirp_name *base, uint64_t version) {
    if (!ctx || !base) return false;

    pubsub_topic *t = find_topic(ctx, base);
    if (!t) {
        if (ctx->topic_count >= PUBSUB_MAX_TOPICS) return false;
        t = &ctx->topics[ctx->topic_count++];
        memset(t, 0, sizeof(*t));
        /* copy base name (components owned by caller; re-init from its strings) */
        char *comps[PSIRP_MAX_COMPONENTS];
        for (size_t i = 0; i < base->count; i++) {
            comps[i] = (char *)malloc(strlen(base->components[i]) + 1);
            strcpy(comps[i], base->components[i]);
        }
        psirp_name_init_components(&t->base, (const char **)comps, base->count);
        for (size_t i = 0; i < base->count; i++) free(comps[i]);
        t->active = true;
        t->highest = version;
    } else if (version <= t->highest) {
        return false; /* not newer */
    } else {
        t->highest = version;
    }

    /* Announce to DHT: name hash -> self as holder. */
    if (ctx->dht) {
        uint32_t ip = htonl(ctx->self_ip);
        uint16_t port = htons(ctx->self_port);
        dht_announce(ctx->dht, base->hash, ip, port);
    }
    return true;
}

void pubsub_subscribe(pubsub_ctx *ctx, const psirp_name *base) {
    if (!ctx || !base) return;
    if (find_topic(ctx, base)) return;
    if (ctx->topic_count >= PUBSUB_MAX_TOPICS) return;
    pubsub_topic *t = &ctx->topics[ctx->topic_count++];
    memset(t, 0, sizeof(*t));
    char *comps[PSIRP_MAX_COMPONENTS];
    for (size_t i = 0; i < base->count; i++) {
        comps[i] = (char *)malloc(strlen(base->components[i]) + 1);
        strcpy(comps[i], base->components[i]);
    }
    psirp_name_init_components(&t->base, (const char **)comps, base->count);
    for (size_t i = 0; i < base->count; i++) free(comps[i]);
    t->active = true;
    /* highest stays 0 until we learn a version */
}

bool pubsub_poll(pubsub_ctx *ctx, const psirp_name *base, uint64_t *out_version) {
    if (!ctx || !base) return false;
    pubsub_topic *t = find_topic(ctx, base);
    if (!t || t->highest == 0) return false;
    /* Pull version from DHT if available and newer. */
    if (ctx->dht) {
        uint32_t ips[DHT_MAX_HOLDERS];
        uint16_t ports[DHT_MAX_HOLDERS];
        size_t n = dht_lookup(ctx->dht, base->hash, ips, ports);
        if (n > 0) {
            /* DHT doesn't carry version; rely on local table for now. */
        }
    }
    if (out_version) *out_version = t->highest;
    return t->highest > 0;
}

bool pubsub_versioned_name(const psirp_name *base, uint64_t version, psirp_name *out) {
    if (!base || !out) return false;
    char path[PSIRP_MAX_NAME];
    char base_str[PSIRP_MAX_NAME];
    psirp_name_to_string(base, base_str, sizeof(base_str));
    snprintf(path, sizeof(path), "%s@%llu", base_str, (unsigned long long)version);
    return psirp_name_init(out, path);
}
