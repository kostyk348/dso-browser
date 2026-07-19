/**
 * @file dso_psirp.c
 * @brief DSO-PSIRP Integration implementation.
 *
 * Uses DSO runtime API:
 *   dso_context_init(ctx, arena)
 *   dso_task(ctx, name, fn, user, &task_id)
 *   dso_dep(ctx, before_id, after_id)
 *   dso_compile(ctx, contract, &verdict)
 *   dso_execute(ctx)
 */

#include "dso_psirp.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char *get_extension(const psirp_name *name) {
    if (name->count == 0) return "";
    const char *last = name->components[name->count - 1];
    const char *dot = strrchr(last, '.');
    return dot ? dot : "";
}

// ── Content Type Detection ────────────────────────────────────────────────────

dso_psirp_content_type dso_psirp_detect_type(const psirp_name *name) {
    const char *ext = get_extension(name);

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return DSO_PSIRP_CONTENT_HTML;
    if (strcmp(ext, ".css") == 0) return DSO_PSIRP_CONTENT_CSS;
    if (strcmp(ext, ".js") == 0) return DSO_PSIRP_CONTENT_JS;
    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 ||
        strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".gif") == 0 ||
        strcmp(ext, ".webp") == 0 || strcmp(ext, ".svg") == 0) return DSO_PSIRP_CONTENT_IMAGE;

    return DSO_PSIRP_CONTENT_UNKNOWN;
}

dso_psirp_content_type dso_psirp_detect_type_data(const uint8_t *data, size_t len) {
    if (!data || len < 4) return DSO_PSIRP_CONTENT_UNKNOWN;

    // PNG
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return DSO_PSIRP_CONTENT_IMAGE;

    // JPEG
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return DSO_PSIRP_CONTENT_IMAGE;

    // GIF
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return DSO_PSIRP_CONTENT_IMAGE;

    // HTML
    if (len > 15) {
        const char *s = (const char *)data;
        if (strstr(s, "<!DOCTYPE") || strstr(s, "<html") || strstr(s, "<HTML"))
            return DSO_PSIRP_CONTENT_HTML;
    }

    // CSS
    if (len > 10) {
        const char *s = (const char *)data;
        if (strstr(s, "@charset") || strstr(s, "selector"))
            return DSO_PSIRP_CONTENT_CSS;
    }

    return DSO_PSIRP_CONTENT_UNKNOWN;
}

// ── Context Init ──────────────────────────────────────────────────────────────

void dso_psirp_ctx_init(dso_psirp_ctx *ctx, psirp_node *net, psirp_cs *cs,
                        dso_arena *arena) {
    if (!ctx) return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->net = net;
    ctx->cs = cs;
    ctx->arena = arena;

    // Initialize DSO context with the arena
    ctx->dso_ctx = (dso_context *)dso_arena_alloc(arena, sizeof(dso_context), 8);
    if (ctx->dso_ctx) {
        dso_context_init(ctx->dso_ctx, arena);
    }
}

// ── DSO Task Implementations ──────────────────────────────────────────────────

void dso_psirp_task_fetch(void *user) {
    dso_psirp_ctx *ctx = (dso_psirp_ctx *)user;
    if (!ctx || !ctx->net) return;

    uint64_t start = now_ns();

    // Fetch main page
    const psirp_cs_entry *entry = psirp_net_fetch(ctx->net, &ctx->page_name,
                                                   DSO_PSIRP_FETCH_TIMEOUT);

    if (entry && ctx->item_count < 64) {
        dso_psirp_content *item = &ctx->items[ctx->item_count++];

        // Copy name components
        psirp_name_init_components(&item->name, entry->name.components, entry->name.count);
        item->type = dso_psirp_detect_type(&entry->name);
        item->data = entry->data;
        item->data_len = entry->data_len;
        item->resolved = true;

        if (item->type == DSO_PSIRP_CONTENT_HTML) {
            ctx->page_html = item;
        }
    }

    ctx->fetch_time_ns += now_ns() - start;
}

void dso_psirp_task_parse(void *user) {
    dso_psirp_ctx *ctx = (dso_psirp_ctx *)user;
    if (!ctx || !ctx->page_html) return;

    uint64_t start = now_ns();
    // TODO: proper HTML parsing for sub-resource references
    ctx->parse_time_ns += now_ns() - start;
}

void dso_psirp_task_resolve_css(void *user) {
    dso_psirp_ctx *ctx = (dso_psirp_ctx *)user;
    if (!ctx || !ctx->net) return;

    uint64_t start = now_ns();
    // TODO: parse CSS refs from HTML, fetch each
    ctx->parse_time_ns += now_ns() - start;
}

void dso_psirp_task_resolve_images(void *user) {
    dso_psirp_ctx *ctx = (dso_psirp_ctx *)user;
    if (!ctx || !ctx->net) return;

    uint64_t start = now_ns();
    // TODO: parse image refs from HTML, fetch each
    ctx->parse_time_ns += now_ns() - start;
}

void dso_psirp_task_render(void *user) {
    dso_psirp_ctx *ctx = (dso_psirp_ctx *)user;
    if (!ctx || !ctx->page_html) return;

    uint64_t start = now_ns();
    // TODO: render HTML → bitmap (via WebKit or custom renderer)
    ctx->render_time_ns += now_ns() - start;
}

// ── Task Graph Builder ────────────────────────────────────────────────────────

bool dso_psirp_build_graph(dso_psirp_ctx *ctx, const psirp_name *page_name) {
    if (!ctx || !page_name || !ctx->dso_ctx) return false;

    dso_context *dso = ctx->dso_ctx;

    // Copy page name
    ctx->page_name = *page_name;

    // Reset context for new graph
    dso_arena_reset(ctx->arena);

    // Re-init DSO context (clear old tasks/deps)
    dso_context_init(dso, ctx->arena);

    // Register tasks — dso_task returns ID via out_id
    uint32_t id_fetch, id_parse, id_css, id_images, id_render;

    dso_task(dso, "fetch_html",   dso_psirp_task_fetch,        ctx, &id_fetch);
    dso_task(dso, "parse_html",   dso_psirp_task_parse,        ctx, &id_parse);
    dso_task(dso, "fetch_css",    dso_psirp_task_resolve_css,  ctx, &id_css);
    dso_task(dso, "fetch_images", dso_psirp_task_resolve_images, ctx, &id_images);
    dso_task(dso, "render",       dso_psirp_task_render,       ctx, &id_render);

    // Dependencies (by ID)
    dso_dep(dso, id_fetch,  id_parse);    // parse waits for fetch
    dso_dep(dso, id_parse,  id_css);      // fetch_css waits for parse
    dso_dep(dso, id_parse,  id_images);   // fetch_images waits for parse
    dso_dep(dso, id_css,    id_render);   // render waits for css
    dso_dep(dso, id_images, id_render);   // render waits for images

    // Compile with global frame contract (16ms budget = 60fps)
    dso_contract frame_contract = {
        .max_tasks       = DSO_PSIRP_MAX_TASKS,
        .max_deps        = 512,
        .max_arena_bytes = ctx->arena->capacity,
        .max_plan_steps  = DSO_PSIRP_MAX_TASKS,
        .max_step_ns     = DSO_PSIRP_FETCH_TIMEOUT * 1000000ULL,  // 4s per step max
        .max_frame_ns    = 50 * 1000000ULL,  // 50ms total frame budget
    };

    dso_verdict verdict;
    dso_status status = dso_compile(dso, frame_contract, &verdict);

    if (status != DSO_OK) {
        fprintf(stderr, "DSO compile failed: %s\n", verdict.message);
        return false;
    }

    // Set per-task resource contracts
    dso_resource_contract rc_fetch = {
        .wcet_us_max = DSO_PSIRP_FETCH_TIMEOUT * 1000.0,
        .jitter_us_max = 1000.0,
    };
    dso_task_set_contract(dso, id_fetch, rc_fetch);

    dso_resource_contract rc_render = {
        .wcet_us_max = DSO_PSIRP_RENDER_BUDGET * 1000.0,
        .jitter_us_max = 100.0,
    };
    dso_task_set_contract(dso, id_render, rc_render);

    return true;
}

// ── Fetch Pipeline ────────────────────────────────────────────────────────────

bool dso_psirp_fetch_page(dso_psirp_ctx *ctx, const psirp_name *page_name,
                          uint32_t timeout_ms) {
    if (!ctx || !page_name) return false;

    uint64_t start = now_ns();

    // Build task graph
    if (!dso_psirp_build_graph(ctx, page_name)) {
        return false;
    }

    // Execute task graph
    dso_status status = dso_execute(ctx->dso_ctx);

    ctx->fetch_time_ns = now_ns() - start;

    return status == DSO_OK;
}

// ── Content Accessors ─────────────────────────────────────────────────────────

const dso_psirp_content *dso_psirp_get_html(const dso_psirp_ctx *ctx) {
    return ctx ? ctx->page_html : NULL;
}

const dso_psirp_content *dso_psirp_get_css(const dso_psirp_ctx *ctx, size_t index) {
    if (!ctx) return NULL;

    size_t css_count = 0;
    for (size_t i = 0; i < ctx->item_count; i++) {
        if (ctx->items[i].type == DSO_PSIRP_CONTENT_CSS) {
            if (css_count == index) return &ctx->items[i];
            css_count++;
        }
    }
    return NULL;
}

const dso_psirp_content *dso_psirp_get_image(const dso_psirp_ctx *ctx, size_t index) {
    if (!ctx) return NULL;

    size_t img_count = 0;
    for (size_t i = 0; i < ctx->item_count; i++) {
        if (ctx->items[i].type == DSO_PSIRP_CONTENT_IMAGE) {
            if (img_count == index) return &ctx->items[i];
            img_count++;
        }
    }
    return NULL;
}
