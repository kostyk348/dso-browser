/**
 * @file pubsub.h
 * @brief Publish/Subscribe layer over PSIRP + DHT.
 *
 * Solves the "dynamic site" problem: a content name like /news is stable,
 * but its data changes over time. We address versions explicitly:
 *
 *   /news          -> "latest" (subscriber pulls highest known version)
 *   /news@42       -> specific version (content-addressable, cacheable)
 *
 * Publishers announce new versions into the DHT (so holders + version are
 * discoverable). Subscribers watch the topic and fetch /news@<newest>.
 */

#ifndef PSIRP_PUBSUB_H
#define PSIRP_PUBSUB_H

#include "psirp.h"
#include "dht.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUBSUB_MAX_TOPICS 256

/** @brief A subscribed/published topic tracker. */
typedef struct {
    psirp_name  base;        ///< Base name (no version), e.g. /news
    uint64_t    highest;     ///< Highest version seen
    bool        active;
} pubsub_topic;

/** @brief Pub/Sub context. */
typedef struct {
    dht_node   *dht;            ///< DHT for holder/version discovery (may be NULL)
    pubsub_topic topics[PUBSUB_MAX_TOPICS];
    size_t      topic_count;
    uint32_t    self_ip;        ///< Our reachable IP (host order)
    uint16_t    self_port;      ///< Our reachable port (host order)
} pubsub_ctx;

void pubsub_init(pubsub_ctx *ctx, dht_node *dht, uint32_t self_ip, uint16_t self_port);

/**
 * @brief Publisher: announce a new version of a topic.
 *
 * Stores the version in the local table and announces (hash -> self) to the
 * DHT so subscribers can discover both the version and a holder.
 *
 * @return true if version is newer than last announced
 */
bool pubsub_publish(pubsub_ctx *ctx, const psirp_name *base, uint64_t version);

/**
 * @brief Subscriber: register interest in a topic.
 */
void pubsub_subscribe(pubsub_ctx *ctx, const psirp_name *base);

/**
 * @brief Check whether a newer version than last seen exists locally.
 *
 * @param out_version  Filled with the newest version if return is true
 * @return true if a newer version is available
 */
bool pubsub_poll(pubsub_ctx *ctx, const psirp_name *base, uint64_t *out_version);

/**
 * @brief Build the versioned name for a topic at a given version.
 *        e.g. base=/news, version=42 -> /news@42
 */
bool pubsub_versioned_name(const psirp_name *base, uint64_t version, psirp_name *out);

#ifdef __cplusplus
}
#endif

#endif // PSIRP_PUBSUB_H
