/**
 * @file html_parse.c
 * @brief Lightweight HTML parser for sub-resource extraction.
 *
 * Design:
 * - Zero allocation — caller provides all buffers
 * - Single-pass scanner — O(n) where n = html_len
 * - Converts relative URLs to PSIRP content names
 * - Handles <link>, <script>, <img>, <a>, <style>, <meta>
 */

#include "html_parse.h"
#include <string.h>
#include <ctype.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static size_t skip_whitespace(const char *s, size_t i, size_t len) {
    while (i < len && isspace((unsigned char)s[i])) i++;
    return i;
}

static size_t skip_to(const char *s, size_t i, size_t len, char c) {
    while (i < len && s[i] != c) i++;
    return i;
}

static size_t skip_to_case(const char *s, size_t i, size_t len, const char *stop, size_t stop_len) {
    while (i + stop_len <= len) {
        if (strncasecmp(s + i, stop, stop_len) == 0) return i;
        i++;
    }
    return len;
}

/**
 * @brief Extract attribute value from tag.
 *
 * Supports: attr="value", attr='value', attr=value
 */
static size_t extract_attr(const char *tag, size_t tag_len,
                           const char *attr_name, size_t attr_name_len,
                           char *out, size_t out_len) {
    size_t i = 0;
    
    while (i < tag_len) {
        // Skip whitespace
        while (i < tag_len && isspace((unsigned char)tag[i])) i++;
        if (i >= tag_len) break;
        
        // Check if attribute matches
        if (strncasecmp(tag + i, attr_name, attr_name_len) == 0) {
            i += attr_name_len;
            if (i >= tag_len) break;
            
            // Skip whitespace
            while (i < tag_len && isspace((unsigned char)tag[i])) i++;
            if (i >= tag_len || tag[i] != '=') continue;
            i++; // skip '='
            
            // Skip whitespace
            while (i < tag_len && isspace((unsigned char)tag[i])) i++;
            if (i >= tag_len) break;
            
            // Extract quoted or unquoted value
            char quote = tag[i];
            if (quote == '"' || quote == '\'') {
                i++; // skip opening quote
                size_t j = i;
                while (j < tag_len && tag[j] != quote) j++;
                size_t vlen = j - i;
                if (vlen >= out_len) vlen = out_len - 1;
                memcpy(out, tag + i, vlen);
                out[vlen] = '\0';
                return vlen;
            } else {
                // Unquoted value
                size_t j = i;
                while (j < tag_len && !isspace((unsigned char)tag[j]) && tag[j] != '>' && tag[j] != '/') j++;
                size_t vlen = j - i;
                if (vlen >= out_len) vlen = out_len - 1;
                memcpy(out, tag + i, vlen);
                out[vlen] = '\0';
                return vlen;
            }
        }
        
        // Skip to next attribute
        while (i < tag_len && !isspace((unsigned char)tag[i])) i++;
    }
    
    out[0] = '\0';
    return 0;
}

/**
 * @brief Check if tag is self-closing.
 */
static bool is_self_closing(const char *tag, size_t tag_len) {
    if (tag_len == 0) return false;
    return tag[tag_len - 1] == '/';
}

/**
 * @brief Get tag name from raw tag content.
 */
static size_t extract_tag_name(const char *tag, size_t tag_len,
                               char *name, size_t name_len) {
    size_t i = 0;
    // Skip '<'
    if (i < tag_len && tag[i] == '<') i++;
    // Skip '/' for closing tags
    if (i < tag_len && tag[i] == '/') i++;
    // Extract name
    size_t j = i;
    while (j < tag_len && !isspace((unsigned char)tag[j]) && tag[j] != '>' && tag[j] != '/') j++;
    size_t nlen = j - i;
    if (nlen >= name_len) nlen = name_len - 1;
    memcpy(name, tag + i, nlen);
    name[nlen] = '\0';
    return nlen;
}

// ── URL Resolution ────────────────────────────────────────────────────────────

size_t html_resolve_url(const char *base_name, const char *rel_url,
                        char *out, size_t out_len) {
    if (!rel_url || !out || out_len == 0) return 0;
    
    size_t rlen = strlen(rel_url);
    
    // Absolute URL → extract path
    if (rlen >= 7 && strncasecmp(rel_url, "http://", 7) == 0) {
        const char *path = rel_url + 7;
        // Skip host
        while (*path && *path != '/') path++;
        size_t plen = strlen(path);
        if (plen >= out_len) plen = out_len - 1;
        memcpy(out, path, plen);
        out[plen] = '\0';
        return plen;
    }
    if (rlen >= 8 && strncasecmp(rel_url, "https://", 8) == 0) {
        const char *path = rel_url + 8;
        while (*path && *path != '/') path++;
        size_t plen = strlen(path);
        if (plen >= out_len) plen = out_len - 1;
        memcpy(out, path, plen);
        out[plen] = '\0';
        return plen;
    }
    
    // Absolute path → use as-is
    if (rel_url[0] == '/') {
        size_t cplen = rlen;
        if (cplen >= out_len) cplen = out_len - 1;
        memcpy(out, rel_url, cplen);
        out[cplen] = '\0';
        return cplen;
    }
    
    // Relative path → resolve against base
    if (!base_name) {
        size_t cplen = rlen;
        if (cplen >= out_len) cplen = out_len - 1;
        memcpy(out, rel_url, cplen);
        out[cplen] = '\0';
        return cplen;
    }
    
    // Find last '/' in base
    size_t blen = strlen(base_name);
    size_t last_slash = 0;
    for (size_t i = 0; i < blen; i++) {
        if (base_name[i] == '/') last_slash = i;
    }
    
    // Build: base[0..last_slash] + rel_url
    size_t prefix_len = last_slash + 1;
    size_t total = prefix_len + rlen;
    if (total >= out_len) total = out_len - 1;
    
    memcpy(out, base_name, prefix_len);
    memcpy(out + prefix_len, rel_url, rlen);
    out[total] = '\0';
    
    return total;
}

// ── Type Guessing ─────────────────────────────────────────────────────────────

html_resource_type html_guess_type(const char *url) {
    if (!url) return HTML_RES_LINK;
    
    size_t len = strlen(url);
    
    // Find last '.'
    size_t ext_start = len;
    for (size_t i = len; i > 0; i--) {
        if (url[i-1] == '.') { ext_start = i - 1; break; }
        if (url[i-1] == '/') break;
    }
    
    const char *ext = url + ext_start;
    size_t elen = len - ext_start;
    
    // CSS
    if (elen >= 4 && strncasecmp(ext, ".css", 4) == 0)
        return HTML_RES_STYLESHEET;
    
    // JS
    if (elen >= 3 && strncasecmp(ext, ".js", 3) == 0)
        return HTML_RES_SCRIPT;
    
    // Images
    if ((elen >= 4 && (strncasecmp(ext, ".png", 4) == 0 ||
                       strncasecmp(ext, ".jpg", 4) == 0 ||
                       strncasecmp(ext, ".gif", 4) == 0 ||
                       strncasecmp(ext, ".svg", 4) == 0 ||
                       strncasecmp(ext, ".ico", 4) == 0)) ||
        (elen >= 5 && (strncasecmp(ext, ".webp", 5) == 0 ||
                       strncasecmp(ext, ".avif", 5) == 0 ||
                       strncasecmp(ext, ".jpeg", 5) == 0)))
        return HTML_RES_IMAGE;
    
    return HTML_RES_LINK;
}

// ── Main Parser ───────────────────────────────────────────────────────────────

bool html_parse(const char *html, size_t html_len,
                const char *base_name,
                html_parse_result *result) {
    if (!html || !result) return false;
    
    memset(result, 0, sizeof(*result));
    
    if (html_len == 0) html_len = strlen(html);
    
    size_t i = 0;
    bool in_style = false;
    size_t style_start = 0;
    
    while (i < html_len && result->resource_count < HTML_MAX_RESOURCES) {
        // Find next '<'
        size_t tag_start = i;
        while (i < html_len && html[i] != '<') i++;
        if (i >= html_len) break;
        
        // Check for closing </style>
        if (in_style) {
            if (strncasecmp(html + i, "</style", 7) == 0) {
                // Extract inline CSS
                size_t css_len = i - style_start;
                if (css_len > HTML_MAX_INLINE_CSS - 1) css_len = HTML_MAX_INLINE_CSS - 1;
                memcpy(result->inline_css_buf + result->inline_css_len,
                       html + style_start, css_len);
                result->inline_css_buf[result->inline_css_len + css_len] = '\0';
                
                result->resources[result->resource_count].type = HTML_RES_INLINE_CSS;
                result->resources[result->resource_count].inline_data =
                    result->inline_css_buf + result->inline_css_len;
                result->resources[result->resource_count].inline_len = css_len;
                result->resource_count++;
                result->inline_css_len += css_len + 1;
                
                in_style = false;
            }
            i++;
            continue;
        }
        
        // Find end of tag '>'
        size_t tag_end = i;
        while (tag_end < html_len && html[tag_end] != '>') tag_end++;
        if (tag_end >= html_len) break;
        
        const char *tag = html + i;
        size_t tag_len = tag_end - i;
        
        // Extract tag name
        char tag_name[HTML_MAX_TAG_LEN];
        extract_tag_name(tag, tag_len, tag_name, sizeof(tag_name));
        
        // Skip closing tags (</...>)
        bool is_closing = (tag_len > 1 && tag[1] == '/');
        if (is_closing) {
            i = tag_end + 1;
            continue;
        }
        
        // ── <title> ────────────────────────────────────────────────────────
        if (strcasecmp(tag_name, "title") == 0 && !is_self_closing(tag, tag_len)) {
            size_t title_start = tag_end + 1;
            size_t title_end = skip_to_case(html, title_start, html_len, "</title", 6);
            result->title = (char *)(html + title_start);
            result->title_len = title_end - title_start;
        }
        // ── <base href="..."> ─────────────────────────────────────────────
        else if (strcasecmp(tag_name, "base") == 0) {
            char href[HTML_MAX_ATTR_LEN];
            extract_attr(tag, tag_len, "href", 4, href, sizeof(href));
            if (href[0]) {
                result->base_href = result->inline_css_buf; // reuse buffer area
                size_t hlen = strlen(href);
                if (hlen >= HTML_MAX_INLINE_CSS) hlen = HTML_MAX_INLINE_CSS - 1;
                memcpy(result->base_href, href, hlen);
                result->base_href[hlen] = '\0';
                result->base_href_len = hlen;
            }
        }
        // ── <link rel="stylesheet" href="..."> ────────────────────────────
        else if (strcasecmp(tag_name, "link") == 0) {
            char rel[64], href[HTML_MAX_ATTR_LEN];
            extract_attr(tag, tag_len, "rel", 3, rel, sizeof(rel));
            extract_attr(tag, tag_len, "href", 4, href, sizeof(href));
            
            if (href[0]) {
                html_resource *res = &result->resources[result->resource_count];
                res->type = (strcasecmp(rel, "stylesheet") == 0) ?
                            HTML_RES_STYLESHEET : HTML_RES_LINK;
                strncpy(res->raw_url, href, HTML_MAX_ATTR_LEN - 1);
                html_resolve_url(base_name, href, res->name, HTML_MAX_ATTR_LEN);
                result->resource_count++;
            }
        }
        // ── <script src="..."> ────────────────────────────────────────────
        else if (strcasecmp(tag_name, "script") == 0) {
            char src[HTML_MAX_ATTR_LEN];
            extract_attr(tag, tag_len, "src", 3, src, sizeof(src));
            
            if (src[0]) {
                html_resource *res = &result->resources[result->resource_count];
                res->type = HTML_RES_SCRIPT;
                strncpy(res->raw_url, src, HTML_MAX_ATTR_LEN - 1);
                html_resolve_url(base_name, src, res->name, HTML_MAX_ATTR_LEN);
                result->resource_count++;
            }
        }
        // ── <img src="..."> ───────────────────────────────────────────────
        else if (strcasecmp(tag_name, "img") == 0) {
            char src[HTML_MAX_ATTR_LEN];
            extract_attr(tag, tag_len, "src", 3, src, sizeof(src));
            
            if (src[0]) {
                html_resource *res = &result->resources[result->resource_count];
                res->type = HTML_RES_IMAGE;
                strncpy(res->raw_url, src, HTML_MAX_ATTR_LEN - 1);
                html_resolve_url(base_name, src, res->name, HTML_MAX_ATTR_LEN);
                result->resource_count++;
            }
        }
        // ── <a href="..."> ────────────────────────────────────────────────
        else if (strcasecmp(tag_name, "a") == 0) {
            char href[HTML_MAX_ATTR_LEN];
            extract_attr(tag, tag_len, "href", 4, href, sizeof(href));
            
            if (href[0]) {
                html_resource *res = &result->resources[result->resource_count];
                res->type = HTML_RES_LINK;
                strncpy(res->raw_url, href, HTML_MAX_ATTR_LEN - 1);
                html_resolve_url(base_name, href, res->name, HTML_MAX_ATTR_LEN);
                result->resource_count++;
            }
        }
        // ── <style>...</style> ────────────────────────────────────────────
        else if (strcasecmp(tag_name, "style") == 0 && !is_self_closing(tag, tag_len)) {
            in_style = true;
            style_start = tag_end + 1;
        }
        
        // Check for self-closing tags
        if (is_self_closing(tag, tag_len)) {
            i = tag_end + 1;
            continue;
        }
        
        i = tag_end + 1;
    }
    
    return true;
}
