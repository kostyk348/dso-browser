/**
 * @file compute.h
 * @brief Compute-on-peer: dynamic content rendered by an executor peer.
 *
 * Closes the "dynamic site / DB-backed page" gap. Instead of fetching a
 * static object, a request carries parameters; an executor peer runs a
 * registered handler (a DSO task graph) and returns a *signed* result.
 * The result is verifiable by the requester via the executor's Ed25519 key.
 *
 * Wire packets: PSIRP_PKT_COMPUTE_REQ / PSIRP_PKT_COMPUTE_RESP.
 */

#ifndef PSIRP_COMPUTE_H
#define PSIRP_COMPUTE_H

#include "psirp.h"
#include "signing.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSIRP_COMPUTE_MAX_PARAMS 4096
#define PSIRP_COMPUTE_MAX_RESULT (1 << 20)

/** @brief Compute request. */
typedef struct {
    psirp_name  name;       ///< Resource name (e.g. /shop/render)
    uint8_t     params[PSIRP_COMPUTE_MAX_PARAMS];
    size_t      params_len;
    uint64_t    nonce;
} psirp_compute_req;

/** @brief Compute response (signed by executor). */
typedef struct {
    psirp_name  name;       ///< Resource name
    uint8_t     result[PSIRP_COMPUTE_MAX_RESULT];
    size_t      result_len;
    uint8_t     signature[PSIRP_SIGN_SIZE];   ///< Ed25519 over result
    uint8_t     pubkey[PSIRP_KEY_SIZE];        ///< Executor's public key
} psirp_compute_resp;

/** @brief Executor handler: render(params, len) -> output into out (max out_cap). */
typedef size_t (*psirp_compute_handler)(const uint8_t *params, size_t params_len,
                                        uint8_t *out, size_t out_cap);

/* ── Serialization (compute packets share the PSIRP wire framing) ─────────── */

size_t psirp_compute_req_serialize(const psirp_compute_req *req, uint8_t *buf, size_t buf_len);
bool   psirp_compute_req_deserialize(psirp_compute_req *req, const uint8_t *buf, size_t buf_len);

size_t psirp_compute_resp_serialize(const psirp_compute_resp *resp, uint8_t *buf, size_t buf_len);
bool   psirp_compute_resp_deserialize(psirp_compute_resp *resp, const uint8_t *buf, size_t buf_len);

/* ── Execution ──────────────────────────────────────────────────────────────── */

/**
 * @brief Execute a request locally with a handler and produce a signed resp.
 *
 * @param req       Request to execute
 * @param handler   Registered handler (renders result)
 * @param kp        Executor's keypair (for signing)
 * @param resp      Output signed response
 * @return true on success
 */
bool psirp_compute_execute(const psirp_compute_req *req, psirp_compute_handler handler,
                           const psirp_keypair *kp, psirp_compute_resp *resp);

/**
 * @brief Verify a compute response's signature against the embedded pubkey.
 */
bool psirp_compute_verify(const psirp_compute_resp *resp);

#ifdef __cplusplus
}
#endif

#endif // PSIRP_COMPUTE_H
