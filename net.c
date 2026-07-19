/**
 * @file net.c
 * @brief PSIRP Network Transport implementation.
 */

#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t random_nonce(void) {
    static uint64_t counter = 0;
    return (uint64_t)time(NULL) ^ (++counter << 32) ^ (uint64_t)rand();
}

// ── Socket Setup ──────────────────────────────────────────────────────────────

static int create_udp_socket(uint16_t port, bool reuse) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    
    if (reuse) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    // Set non-blocking for receiver thread
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    return fd;
}

static int create_multicast_socket(uint16_t port, const char *group) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    // Set non-blocking — critical for select/poll-free recv loop
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = INADDR_ANY;
    
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

// ── Init/Stop ─────────────────────────────────────────────────────────────────

bool psirp_net_init(psirp_node *node, uint16_t port, psirp_cs *cs) {
    if (!node) return false;
    
    memset(node, 0, sizeof(*node));
    node->port = port ? port : PSIRP_DEFAULT_PORT;
    node->cs = cs;
    
    // Create unicast socket
    node->fd = create_udp_socket(node->port, true);
    if (node->fd < 0) return false;
    
    // Create multicast socket
    node->mcast_fd = create_multicast_socket(PSIRP_MULTICAST_PORT, PSIRP_MULTICAST_GROUP);
    // Multicast is optional, don't fail if unavailable
    
    node->running = false;
    return true;
}

void psirp_net_stop(psirp_node *node) {
    if (!node) return;
    
    node->running = false;
    
    if (node->recv_thread) {
        pthread_join(node->recv_thread, NULL);
    }
    
    if (node->fd >= 0) {
        close(node->fd);
        node->fd = -1;
    }
    
    if (node->mcast_fd >= 0) {
        close(node->mcast_fd);
        node->mcast_fd = -1;
    }
}

// ── Receiver Thread ───────────────────────────────────────────────────────────

static void *recv_thread_func(void *arg) {
    psirp_node *node = (psirp_node *)arg;
    uint8_t buf[PSIRP_RECV_BUF_SIZE];
    
    while (node->running) {
        // Check unicast socket
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        
        ssize_t n = recvfrom(node->fd, buf, sizeof(buf), 0,
                            (struct sockaddr *)&from, &from_len);
        
        if (n > 0) {
            uint8_t pkt_type = buf[0];
            
            if (pkt_type == PSIRP_PKT_INTEREST) {
                // Receive interest
                psirp_interest interest = {0};
                if (psirp_interest_deserialize(&interest, buf, (size_t)n)) {
                    // Check if we have it in CS
                    const psirp_cs_entry *entry = psirp_cs_lookup(node->cs, &interest.name);
                    if (entry) {
                        // Send data reply
                        psirp_data reply = {
                            .name = entry->name,
                            .data = entry->data,
                            .data_len = entry->data_len,
                            .timestamp = entry->timestamp,
                            .freshness_ms = entry->freshness_ms
                        };
                        
                        psirp_peer peer = {
                            .ip = from.sin_addr.s_addr,
                            .port = from.sin_port
                        };
                        psirp_net_data(node, &peer, &reply);
                        node->data_sent++;
                    } else {
                        // Forward to best peer
                        const psirp_peer *fwd = psirp_fib_lookup(&node->fib, &interest.name);
                        if (fwd) {
                            // Forward interest
                            psirp_net_interest(node, &interest.name, fwd, interest.lifetime_ms);
                            node->interests_forwarded++;
                        }
                    }
                    
                    // Free name components
                    for (size_t i = 0; i < interest.name.count; i++) {
                        free((void *)interest.name.components[i]);
                    }
                }
                
            } else if (pkt_type == PSIRP_PKT_DATA) {
                // Receive data
                psirp_data data = {0};
                if (psirp_data_deserialize(&data, buf, (size_t)n)) {
                    // Store in content store
                    psirp_cs_store(node->cs, &data.name, data.data, data.data_len,
                                  data.freshness_ms);
                    
                    // Check if we have pending interest for this
                    for (size_t i = 0; i < node->pending_count; i++) {
                        if (node->pending[i].active &&
                            psirp_name_equal(&node->pending[i].name, &data.name)) {
                            node->pending[i].active = false;
                            break;
                        }
                    }
                    
                    // Callback
                    if (node->on_data) {
                        node->on_data(&data, node->on_data_user);
                    }
                    
                    node->data_received++;
                    
                    // Free name components
                    for (size_t i = 0; i < data.name.count; i++) {
                        free((void *)data.name.components[i]);
                    }
                }
            }
        }
        
        // Check multicast for discovery
        if (node->mcast_fd >= 0) {
            from_len = sizeof(from);
            n = recvfrom(node->mcast_fd, buf, sizeof(buf), 0,
                        (struct sockaddr *)&from, &from_len);
            
            if (n > 0 && buf[0] == PSIRP_PKT_PUBLISH) {
                // New peer announcement
                psirp_net_add_peer(node, from.sin_addr.s_addr, from.sin_port);
            }
        }
        
        // Small sleep to avoid busy-wait
        struct timespec ts = { .tv_nsec = 1000000 }; // 1ms
        nanosleep(&ts, NULL);
    }
    
    return NULL;
}

bool psirp_net_start(psirp_node *node) {
    if (!node || node->running) return false;
    
    node->running = true;
    
    if (pthread_create(&node->recv_thread, NULL, recv_thread_func, node) != 0) {
        node->running = false;
        return false;
    }
    
    return true;
}

// ── Send ──────────────────────────────────────────────────────────────────────

uint64_t psirp_net_interest(psirp_node *node, const psirp_name *name,
                            const psirp_peer *peer, uint32_t timeout_ms) {
    if (!node || !name) return 0;
    
    psirp_interest interest = {
        .name = *name,
        .nonce = random_nonce(),
        .lifetime_ms = timeout_ms ? timeout_ms : PSIRP_INTEREST_TIMEOUT_MS
    };
    
    uint8_t buf[4096];
    size_t len = psirp_interest_serialize(&interest, buf, sizeof(buf));
    if (len == 0) return 0;
    
    // Determine destination
    struct sockaddr_in dest = {0};
    
    if (peer) {
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = peer->ip;
        dest.sin_port = peer->port;
    } else {
        // Use FIB
        const psirp_peer *fwd = psirp_fib_lookup(&node->fib, name);
        if (!fwd) return 0;
        
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = fwd->ip;
        dest.sin_port = fwd->port;
    }
    
    // Send
    ssize_t sent = sendto(node->fd, buf, len, 0,
                         (struct sockaddr *)&dest, sizeof(dest));
    
    if (sent < 0) return 0;
    
    // Track pending interest
    if (node->pending_count < 256) {
        psirp_pending *p = &node->pending[node->pending_count++];
        memset(p, 0, sizeof(*p));
        psirp_name_init_components(&p->name, name->components, name->count);
        p->nonce = interest.nonce;
        p->sent_time_ns = now_ns();
        p->lifetime_ms = interest.lifetime_ms;
        p->active = true;
    }
    
    node->interests_sent++;
    return interest.nonce;
}

bool psirp_net_data(psirp_node *node, const psirp_peer *peer,
                    const psirp_data *data) {
    if (!node || !peer || !data) return false;
    
    uint8_t buf[PSIRP_RECV_BUF_SIZE];
    size_t len = psirp_data_serialize(data, buf, sizeof(buf));
    if (len == 0) return false;
    
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = peer->ip,
        .sin_port = peer->port
    };
    
    ssize_t sent = sendto(node->fd, buf, len, 0,
                         (struct sockaddr *)&dest, sizeof(dest));
    
    return sent > 0;
}

// ── Peers ─────────────────────────────────────────────────────────────────────

bool psirp_net_add_peer(psirp_node *node, uint32_t ip, uint16_t port) {
    if (!node || node->peer_count >= PSIRP_MAX_PEERS) return false;
    
    // Store port in NETWORK byte order (for direct use in sin_port)
    uint16_t net_port = htons(port);
    
    // Check if already known
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].ip == ip && node->peers[i].port == net_port) {
            return true; // already known
        }
    }
    
    node->peers[node->peer_count++] = (psirp_peer){
        .ip = ip,
        .port = net_port
    };
    
    return true;
}

// ── Multicast ─────────────────────────────────────────────────────────────────

bool psirp_net_multicast_interest(psirp_node *node, const psirp_name *name) {
    if (!node || !name || node->mcast_fd < 0) return false;
    
    psirp_interest interest = {
        .name = *name,
        .nonce = random_nonce(),
        .lifetime_ms = PSIRP_INTEREST_TIMEOUT_MS
    };
    
    uint8_t buf[4096];
    size_t len = psirp_interest_serialize(&interest, buf, sizeof(buf));
    if (len == 0) return false;
    
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(PSIRP_MULTICAST_PORT),
        .sin_addr.s_addr = inet_addr(PSIRP_MULTICAST_GROUP)
    };
    
    ssize_t sent = sendto(node->mcast_fd, buf, len, 0,
                         (struct sockaddr *)&dest, sizeof(dest));
    
    return sent > 0;
}

// ── Blocking Fetch ────────────────────────────────────────────────────────────

const psirp_cs_entry *psirp_net_fetch(psirp_node *node, const psirp_name *name,
                                      uint32_t timeout_ms) {
    if (!node || !name) return NULL;
    
    // Try local CS first
    const psirp_cs_entry *entry = psirp_cs_lookup(node->cs, name);
    if (entry) return entry;
    
    // Send interest to all known peers
    uint64_t nonce = 0;
    for (size_t i = 0; i < node->peer_count; i++) {
        nonce = psirp_net_interest(node, name, &node->peers[i], timeout_ms);
    }
    
    // Also try multicast
    if (node->peer_count == 0) {
        psirp_net_multicast_interest(node, name);
    }
    
    // Wait for data
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    
    while (now_ns() < deadline) {
        // Check CS again
        entry = psirp_cs_lookup(node->cs, name);
        if (entry) return entry;
        
        // Check pending
        bool still_pending = false;
        for (size_t i = 0; i < node->pending_count; i++) {
            if (node->pending[i].active &&
                psirp_name_equal(&node->pending[i].name, name)) {
                still_pending = true;
                break;
            }
        }
        
        if (!still_pending) break;
        
        // Sleep a bit
        struct timespec ts = { .tv_nsec = 100000 }; // 100us
        nanosleep(&ts, NULL);
    }
    
    return psirp_cs_lookup(node->cs, name);
}

// ── Publish ───────────────────────────────────────────────────────────────────

bool psirp_net_publish(psirp_node *node, const psirp_name *name,
                       const uint8_t *data, size_t data_len,
                       uint32_t freshness_ms) {
    if (!node || !name || !data) return false;
    
    // Store locally
    if (!psirp_cs_store(node->cs, name, data, data_len, freshness_ms)) {
        return false;
    }
    
    // Announce to peers via multicast
    psirp_data announcement = {
        .name = *name,
        .data = data,
        .data_len = data_len,
        .timestamp = now_ns(),
        .freshness_ms = freshness_ms
    };
    
    // Send to all known peers
    for (size_t i = 0; i < node->peer_count; i++) {
        psirp_net_data(node, &node->peers[i], &announcement);
    }
    
    return true;
}
