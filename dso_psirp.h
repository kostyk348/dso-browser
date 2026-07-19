/**
 * @file dso_psirp.h
 * @brief DSO-PSIRP Integration — Deterministic content fetching.
 *
 * Wraps PSIRP operations as DSO task graph nodes:
 * - fetch_interest: Send interest, wait for data
 * - parse_content: Parse HTML/CSS/images
 * - render: Execute rendering pipeline
 *
 * Each task has WCET contract for guaranteed execution time.
 */

#ifndef DSO_PSIRP_H
#define DSO_PSIRP_H

#include "dso.h"
#include "psirp.h"
#include "net.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define DSO_PSIRP_MAX_TASKS      32    ///< Max tasks in fetch pipeline
#define DSO_PSIRP_FETCH_TIMEOUT  4000  ///< Default fetch timeout (ms)
#define DSO_PSIRP_RENDER_BUDGET  16    ///< Render budget per frame (ms)

// ── Pipeline Context ──────────────────────────────────────────────────────────

/** @brief Content types. */
typedef enum {
    DSO_PSIRP_CONTENT_HTML,
    DSO_PSIRP_CONTENT_CSS,
    DSO_PSIRP_CONTENT_JS,
    DSO_PSIRP_CONTENT_IMAGE,
    DSO_PSIRP_CONTENT_DATA,
    DSO_PSIRP_CONTENT_UNKNOWN
} dso_psirp_content_type;

/** @brief Fetched content item. */
typedef struct {
    psirp_name          name;           ///< Content name
    dso_psirp_content_type type;        ///< Content type
    const uint8_t      *data;           ///< Content data (in CS)
    size_t              data_len;       ///< Content length
    bool                resolved;       ///< Successfully fetched
} dso_psirp_content;

/** @brief Fetch pipeline context. */
typedef struct {
    psirp_node     *net;                ///< Network node
    psirp_cs       *cs;                 ///< Content store
    dso_arena      *arena;              ///< Task arena
    
    // Task graph
    dso_context    *dso_ctx;            ///< DSO context
    dso_task_fn     tasks[DSO_PSIRP_MAX_TASKS];
    char            task_names[DSO_PSIRP_MAX_TASKS][DSO_MAX_NAME];
    size_t          task_count;
    
    // Content items
    dso_psirp_content items[64];        ///< Fetched content
    size_t          item_count;
    
    // Current page
    psirp_name      page_name;          ///< Current page name
    dso_psirp_content *page_html;       ///< Main HTML content
    
    // Stats
    uint64_t        fetch_time_ns;      ///< Time spent fetching
    uint64_t        parse_time_ns;      ///< Time spent parsing
    uint64_t        render_time_ns;     ///< Time spent rendering
} dso_psirp_ctx;

// ── Content Type Detection ────────────────────────────────────────────────────

/**
 * @brief Detect content type from name extension.
 */
dso_psirp_content_type dso_psirp_detect_type(const psirp_name *name);

/**
 * @brief Detect content type from data (magic bytes).
 */
dso_psirp_content_type dso_psirp_detect_type_data(const uint8_t *data, size_t len);

// ── Fetch Pipeline ────────────────────────────────────────────────────────────

/**
 * @brief Initialize fetch pipeline context.
 */
void dso_psirp_ctx_init(dso_psirp_ctx *ctx, psirp_node *net, psirp_cs *cs,
                        dso_arena *arena);

/**
 * @brief Fetch page and all sub-resources.
 *
 * Task graph:
 *   fetch_html → parse_css_refs → fetch_css → parse_image_refs → fetch_images → render
 *
 * @param ctx       Pipeline context
 * @param page_name Page content name
 * @param timeout_ms Max total time
 * @return true on success
 */
bool dso_psirp_fetch_page(dso_psirp_ctx *ctx, const psirp_name *page_name,
                          uint32_t timeout_ms);

/**
 * @brief Get fetched HTML content.
 */
const dso_psirp_content *dso_psirp_get_html(const dso_psirp_ctx *ctx);

/**
 * @brief Get fetched CSS content by index.
 */
const dso_psirp_content *dso_psirp_get_css(const dso_psirp_ctx *ctx, size_t index);

/**
 * @brief Get fetched image content by index.
 */
const dso_psirp_content *dso_psirp_get_image(const dso_psirp_ctx *ctx, size_t index);

// ── DSO Task Wrappers ─────────────────────────────────────────────────────────

/**
 * @brief DSO task: Fetch content by name.
 *
 * Contract: WCET 50ms (network RTT + CS lookup)
 *
 * @param user  dso_psirp_ctx*
 */
void dso_psirp_task_fetch(void *user);

/**
 * @brief DSO task: Parse HTML for sub-resources.
 *
 * Contract: WCET 1ms (parsing local data)
 *
 * @param user  dso_psirp_ctx*
 */
void dso_psirp_task_parse(void *user);

/**
 * @brief DSO task: Resolve CSS references.
 *
 * Contract: WCET 2ms (parsing + fetching)
 *
 * @param user  dso_psirp_ctx*
 */
void dso_psirp_task_resolve_css(void *user);

/**
 * @brief DSO task: Resolve image references.
 *
 * Contract: WCET 5ms (parsing + fetching)
 *
 * @param user  dso_psirp_ctx*
 */
void dso_psirp_task_resolve_images(void *user);

/**
 * @brief DSO task: Render page to bitmap.
 *
 * Contract: WCET 16ms (60fps budget)
 *
 * @param user  dso_psirp_ctx*
 */
void dso_psirp_task_render(void *user);

// ── Task Graph Builder ────────────────────────────────────────────────────────

/**
 * @brief Build task graph for page fetch pipeline.
 *
 * Creates deterministic task graph with WCET contracts:
 *
 *   Level 0: fetch_html
 *   Level 1: parse_html (depends on fetch_html)
 *   Level 2: fetch_css + fetch_images (depends on parse_html)
 *   Level 3: render (depends on fetch_css + fetch_images)
 *
 * @param ctx       Pipeline context
 * @param page_name Page to fetch
 * @return true on success
 */
bool dso_psirp_build_graph(dso_psirp_ctx *ctx, const psirp_name *page_name);

#ifdef __cplusplus
}
#endif

#endif // DSO_PSIRP_H
