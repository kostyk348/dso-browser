/**
 * @file compute.c
 * @brief Compute-on-peer implementation (see compute.h).
 */

#include "compute.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

// ── Request serialization ──────────────────────────────────────────────────────

size_t psirp_compute_req_serialize(const psirp_compute_req *req, uint8_t *buf, size_t buf_len) {
    if (!req || !buf) return 0;
    size_t pos = 0;
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = PSIRP_PKT_COMPUTE_REQ;
    if (pos + 8 > buf_len) return 0;
    uint64_t nonce = htobe64(req->nonce);
    memcpy(buf + pos, &nonce, 8); pos += 8;
    if (pos + 4 > buf_len) return 0;
    uint32_t plen = htonl((uint32_t)req->params_len);
    memcpy(buf + pos, &plen, 4); pos += 4;
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = (uint8_t)req->name.count;
    for (size_t i = 0; i < req->name.count; i++) {
        size_t cl = strlen(req->name.components[i]);
        if (cl > 255) return 0;
        if (pos + 1 + cl > buf_len) return 0;
        buf[pos++] = (uint8_t)cl;
        memcpy(buf + pos, req->name.components[i], cl); pos += cl;
    }
    if (pos + req->params_len > buf_len) return 0;
    memcpy(buf + pos, req->params, req->params_len); pos += req->params_len;
    return pos;
}

bool psirp_compute_req_deserialize(psirp_compute_req *req, const uint8_t *buf, size_t buf_len) {
    if (!req || !buf || buf_len < 14) return false;
    size_t pos = 0;
    if (buf[pos++] != PSIRP_PKT_COMPUTE_REQ) return false;
    uint64_t nonce; memcpy(&nonce, buf + pos, 8); req->nonce = be64toh(nonce); pos += 8;
    uint32_t plen; memcpy(&plen, buf + pos, 4); req->params_len = ntohl(plen); pos += 4;
    if (pos + 1 > buf_len) return false;
    req->name.count = buf[pos++];
    if (req->name.count > PSIRP_MAX_COMPONENTS) return false;
    char hb[PSIRP_MAX_NAME]; size_t hp = 0;
    for (size_t i = 0; i < req->name.count; i++) {
        if (pos + 1 > buf_len) return false;
        uint8_t cl = buf[pos++];
        if (pos + cl > buf_len || cl >= PSIRP_MAX_COMPONENT) return false;
        char *c = (char *)malloc(cl + 1); memcpy(c, buf + pos, cl); c[cl] = '\0';
        req->name.components[i] = c; pos += cl;
        if (hp + 1 + cl < PSIRP_MAX_NAME) { hb[hp++] = '/'; memcpy(hb + hp, c, cl); hp += cl; }
    }
    req->name.hash = 0; /* recompute lazily if needed */
    (void)hb;
    if (pos + req->params_len > buf_len) return false;
    if (req->params_len > PSIRP_COMPUTE_MAX_PARAMS) return false;
    memcpy(req->params, buf + pos, req->params_len); pos += req->params_len;
    return true;
}

// ── Response serialization ────────────────────────────────────────────────────

size_t psirp_compute_resp_serialize(const psirp_compute_resp *resp, uint8_t *buf, size_t buf_len) {
    if (!resp || !buf) return 0;
    size_t pos = 0;
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = PSIRP_PKT_COMPUTE_RESP;
    if (pos + 4 > buf_len) return 0;
    uint32_t rlen = htonl((uint32_t)resp->result_len);
    memcpy(buf + pos, &rlen, 4); pos += 4;
    if (pos + resp->result_len > buf_len) return 0;
    memcpy(buf + pos, resp->result, resp->result_len); pos += resp->result_len;
    if (pos + PSIRP_SIGN_SIZE > buf_len) return 0;
    memcpy(buf + pos, resp->signature, PSIRP_SIGN_SIZE); pos += PSIRP_SIGN_SIZE;
    if (pos + PSIRP_KEY_SIZE > buf_len) return 0;
    memcpy(buf + pos, resp->pubkey, PSIRP_KEY_SIZE); pos += PSIRP_KEY_SIZE;
    if (pos + 1 > buf_len) return 0;
    buf[pos++] = (uint8_t)resp->name.count;
    for (size_t i = 0; i < resp->name.count; i++) {
        size_t cl = strlen(resp->name.components[i]);
        if (cl > 255) return 0;
        if (pos + 1 + cl > buf_len) return 0;
        buf[pos++] = (uint8_t)cl;
        memcpy(buf + pos, resp->name.components[i], cl); pos += cl;
    }
    return pos;
}

bool psirp_compute_resp_deserialize(psirp_compute_resp *resp, const uint8_t *buf, size_t buf_len) {
    if (!resp || !buf || buf_len < 10) return false;
    size_t pos = 0;
    if (buf[pos++] != PSIRP_PKT_COMPUTE_RESP) return false;
    uint32_t rlen; memcpy(&rlen, buf + pos, 4); resp->result_len = ntohl(rlen); pos += 4;
    if (pos + resp->result_len > buf_len) return false;
    if (resp->result_len > PSIRP_COMPUTE_MAX_RESULT) return false;
    memcpy(resp->result, buf + pos, resp->result_len); pos += resp->result_len;
    if (pos + PSIRP_SIGN_SIZE > buf_len) return false;
    memcpy(resp->signature, buf + pos, PSIRP_SIGN_SIZE); pos += PSIRP_SIGN_SIZE;
    if (pos + PSIRP_KEY_SIZE > buf_len) return false;
    memcpy(resp->pubkey, buf + pos, PSIRP_KEY_SIZE); pos += PSIRP_KEY_SIZE;
    if (pos + 1 > buf_len) return false;
    resp->name.count = buf[pos++];
    if (resp->name.count > PSIRP_MAX_COMPONENTS) return false;
    for (size_t i = 0; i < resp->name.count; i++) {
        if (pos + 1 > buf_len) return false;
        uint8_t cl = buf[pos++];
        if (pos + cl > buf_len || cl >= PSIRP_MAX_COMPONENT) return false;
        char *c = (char *)malloc(cl + 1); memcpy(c, buf + pos, cl); c[cl] = '\0';
        resp->name.components[i] = c; pos += cl;
    }
    return true;
}

// ── Execution ─────────────────────────────────────────────────────────────────

bool psirp_compute_execute(const psirp_compute_req *req, psirp_compute_handler handler,
                           const psirp_keypair *kp, psirp_compute_resp *resp) {
    if (!req || !handler || !kp || !resp) return false;
    memset(resp, 0, sizeof(*resp));

    size_t n = handler(req->params, req->params_len, resp->result, PSIRP_COMPUTE_MAX_RESULT);
    if (n == 0 && req->params_len > 0) return false;
    resp->result_len = n;

    /* Copy name. */
    for (size_t i = 0; i < req->name.count; i++) {
        resp->name.components[i] = (char *)malloc(strlen(req->name.components[i]) + 1);
        strcpy((char *)resp->name.components[i], req->name.components[i]);
    }
    resp->name.count = req->name.count;

    psirp_sign(kp->secret_key, resp->result, n, resp->signature);
    memcpy(resp->pubkey, kp->public_key, PSIRP_KEY_SIZE);
    return true;
}

bool psirp_compute_verify(const psirp_compute_resp *resp) {
    if (!resp) return false;
    return psirp_verify(resp->pubkey, resp->result, resp->result_len, resp->signature);
}
