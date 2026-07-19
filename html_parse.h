/**
 * @file html_parse.h
 * @brief Lightweight HTML parser for sub-resource extraction.
 *
 * Extracts:
 * - <link href="..."> (stylesheets)
 * - <script src="..."> (scripts)
 * - <img src="..."> (images)
 * - <a href="..."> (links/navigation)
 * - <style>...</style> (inline CSS)
 * - <meta> tags
 *
 * Converts relative URLs to PSIRP content names.
 * Zero-allocation design — writes into caller-provided buffer.
 */

#ifndef HTML_PARSE_H
#define HTML_PARSE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define HTML_MAX_RESOURCES   64    ///< Max sub-resources per page
#define HTML_MAX_ATTR_LEN   256   ///< Max attribute value length
#define HTML_MAX_TAG_LEN     64   ///< Max tag name length
#define HTML_MAX_INLINE_CSS 65536 ///< Max inline CSS buffer

// ── Resource Types ────────────────────────────────────────────────────────────

typedef enum {
    HTML_RES_STYLESHEET,    ///< <link rel="stylesheet" href="...">
    HTML_RES_SCRIPT,        ///< <script src="...">
    HTML_RES_IMAGE,         ///< <img src="...">
    HTML_RES_LINK,          ///< <a href="..."> (navigation)
    HTML_RES_INLINE_CSS,    ///< <style>...</style>
    HTML_RES_META           ///< <meta ...>
} html_resource_type;

// ── Extracted Resource ────────────────────────────────────────────────────────

/**
 * @brief A sub-resource extracted from HTML.
 */
typedef struct {
    html_resource_type type;                    ///< Resource type
    char               name[HTML_MAX_ATTR_LEN]; ///< PSIRP content name (resolved)
    char               raw_url[HTML_MAX_ATTR_LEN]; ///< Original URL from HTML
    char              *inline_data;             ///< Pointer to inline CSS (if type == INLINE_CSS)
    size_t             inline_len;              ///< Length of inline CSS
} html_resource;

// ── Parse Result ──────────────────────────────────────────────────────────────

/**
 * @brief Result of HTML parsing.
 */
typedef struct {
    html_resource resources[HTML_MAX_RESOURCES]; ///< Extracted resources
    size_t        resource_count;                ///< Number of resources
    
    char         *title;                         ///< Pointer to <title> content
    size_t        title_len;                     ///< Title length
    
    char         *base_href;                     ///< Pointer to <base href="..."> (if present)
    size_t        base_href_len;                 ///< Base href length
    
    char          inline_css_buf[HTML_MAX_INLINE_CSS]; ///< Buffer for inline CSS
    size_t        inline_css_len;                ///< Total inline CSS length
} html_parse_result;

// ── Parser API ────────────────────────────────────────────────────────────────

/**
 * @brief Parse HTML and extract sub-resources.
 *
 * @param html       HTML content (null-terminated)
 * @param html_len   HTML length (0 = use strlen)
 * @param base_name  Base PSIRP name for resolving relative URLs
 * @param result     Output parse result
 * @return true on success
 */
bool html_parse(const char *html, size_t html_len,
                const char *base_name,
                html_parse_result *result);

/**
 * @brief Resolve a relative URL to a PSIRP content name.
 *
 * Examples:
 *   base="/site/page.html", rel="style.css" → "/site/style.css"
 *   base="/site/page.html", rel="/other.css" → "/other.css"
 *   base="/site/page.html", rel="http://cdn/x.js" → "/cdn/x.js"
 *
 * @param base_name  Base content name
 * @param rel_url    Relative URL
 * @param out        Output buffer
 * @param out_len    Output buffer size
 * @return Number of characters written
 */
size_t html_resolve_url(const char *base_name, const char *rel_url,
                        char *out, size_t out_len);

/**
 * @brief Get resource type from MIME hint or extension.
 */
html_resource_type html_guess_type(const char *url);

#ifdef __cplusplus
}
#endif

#endif // HTML_PARSE_H
