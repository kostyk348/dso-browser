/**
 * @file test_html_parse.c
 * @brief Tests for HTML parser — sub-resource extraction.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "html_parse.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-50s ", name); } while(0)
#define PASS() do { printf("[OK]\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); tests_failed++; } while(0)

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_basic_css_extraction(void) {
    TEST("Extract <link rel=stylesheet>");
    
    const char *html = 
        "<html>\n"
        "<head>\n"
        "  <link rel=\"stylesheet\" href=\"style.css\">\n"
        "  <link rel=\"stylesheet\" href=\"/css/main.css\">\n"
        "</head>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.resource_count == 2);
    assert(result.resources[0].type == HTML_RES_STYLESHEET);
    assert(strcmp(result.resources[0].name, "/site/style.css") == 0);
    assert(result.resources[1].type == HTML_RES_STYLESHEET);
    assert(strcmp(result.resources[1].name, "/css/main.css") == 0);
    
    PASS();
}

static void test_script_extraction(void) {
    TEST("Extract <script src>");
    
    const char *html = 
        "<html>\n"
        "<head>\n"
        "  <script src=\"app.js\"></script>\n"
        "  <script src=\"https://cdn.example.com/lib.js\"></script>\n"
        "</head>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.resource_count == 2);
    assert(result.resources[0].type == HTML_RES_SCRIPT);
    assert(strcmp(result.resources[0].name, "/site/app.js") == 0);
    assert(result.resources[1].type == HTML_RES_SCRIPT);
    assert(strcmp(result.resources[1].name, "/lib.js") == 0);  // https://cdn.example.com stripped
    
    PASS();
}

static void test_image_extraction(void) {
    TEST("Extract <img src>");
    
    const char *html = 
        "<html>\n"
        "<body>\n"
        "  <img src=\"logo.png\">\n"
        "  <img src=\"/images/hero.jpg\">\n"
        "</body>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.resource_count == 2);
    assert(result.resources[0].type == HTML_RES_IMAGE);
    assert(strcmp(result.resources[0].name, "/site/logo.png") == 0);
    assert(result.resources[1].type == HTML_RES_IMAGE);
    assert(strcmp(result.resources[1].name, "/images/hero.jpg") == 0);
    
    PASS();
}

static void test_link_extraction(void) {
    TEST("Extract <a href> links");
    
    const char *html = 
        "<html>\n"
        "<body>\n"
        "  <a href=\"/other/page.html\">Other</a>\n"
        "  <a href=\"sub/page.html\">Sub</a>\n"
        "</body>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.resource_count == 2);
    assert(result.resources[0].type == HTML_RES_LINK);
    assert(strcmp(result.resources[0].name, "/other/page.html") == 0);
    assert(result.resources[1].type == HTML_RES_LINK);
    assert(strcmp(result.resources[1].name, "/site/sub/page.html") == 0);
    
    PASS();
}

static void test_inline_css(void) {
    TEST("Extract <style> inline CSS");
    
    const char *html = 
        "<html>\n"
        "<head>\n"
        "  <style>\n"
        "    body { color: red; }\n"
        "    .box { margin: 10px; }\n"
        "  </style>\n"
        "</head>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.resource_count == 1);
    assert(result.resources[0].type == HTML_RES_INLINE_CSS);
    assert(result.resources[0].inline_len > 0);
    assert(strstr(result.resources[0].inline_data, "color: red") != NULL);
    assert(strstr(result.resources[0].inline_data, "margin: 10px") != NULL);
    
    PASS();
}

static void test_title_extraction(void) {
    TEST("Extract <title>");
    
    const char *html = 
        "<html>\n"
        "<head>\n"
        "  <title>My Page</title>\n"
        "</head>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.title != NULL);
    assert(result.title_len >= 7);
    assert(strncmp(result.title, "My Page", 7) == 0);
    
    PASS();
}

static void test_mixed_content(void) {
    TEST("Mixed content extraction");
    
    const char *html = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "  <title>Mixed</title>\n"
        "  <link rel=\"stylesheet\" href=\"style.css\">\n"
        "  <script src=\"app.js\"></script>\n"
        "  <style>.x { color: blue; }</style>\n"
        "</head>\n"
        "<body>\n"
        "  <h1>Hello</h1>\n"
        "  <img src=\"banner.jpg\">\n"
        "  <a href=\"/about\">About</a>\n"
        "</body>\n"
        "</html>";
    
    html_parse_result result;
    bool ok = html_parse(html, 0, "/site/page.html", &result);
    
    assert(ok);
    assert(result.resource_count == 5);
    
    // Check types
    assert(result.resources[0].type == HTML_RES_STYLESHEET);
    assert(result.resources[1].type == HTML_RES_SCRIPT);
    assert(result.resources[2].type == HTML_RES_INLINE_CSS);
    assert(result.resources[3].type == HTML_RES_IMAGE);
    assert(result.resources[4].type == HTML_RES_LINK);
    
    // Check names
    assert(strcmp(result.resources[0].name, "/site/style.css") == 0);
    assert(strcmp(result.resources[1].name, "/site/app.js") == 0);
    assert(strcmp(result.resources[3].name, "/site/banner.jpg") == 0);
    assert(strcmp(result.resources[4].name, "/about") == 0);
    
    PASS();
}

static void test_url_resolution(void) {
    TEST("URL resolution edge cases");
    
    char buf[256];
    
    // Relative to base
    html_resolve_url("/site/page.html", "style.css", buf, sizeof(buf));
    assert(strcmp(buf, "/site/style.css") == 0);
    
    // Absolute path
    html_resolve_url("/site/page.html", "/other/style.css", buf, sizeof(buf));
    assert(strcmp(buf, "/other/style.css") == 0);
    
    // HTTP URL
    html_resolve_url("/site/page.html", "http://cdn.example.com/lib.js", buf, sizeof(buf));
    assert(strcmp(buf, "/lib.js") == 0);
    
    // HTTPS URL
    html_resolve_url("/site/page.html", "https://cdn.example.com/img.png", buf, sizeof(buf));
    assert(strcmp(buf, "/img.png") == 0);
    
    PASS();
}

static void test_type_guessing(void) {
    TEST("Resource type guessing from extension");
    
    assert(html_guess_type("style.css") == HTML_RES_STYLESHEET);
    assert(html_guess_type("app.js") == HTML_RES_SCRIPT);
    assert(html_guess_type("logo.png") == HTML_RES_IMAGE);
    assert(html_guess_type("photo.jpg") == HTML_RES_IMAGE);
    assert(html_guess_type("icon.svg") == HTML_RES_IMAGE);
    assert(html_guess_type("about.html") == HTML_RES_LINK);
    assert(html_guess_type("/path/to/file.css") == HTML_RES_STYLESHEET);
    
    PASS();
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("html_parse tests:\n");
    
    test_basic_css_extraction();
    test_script_extraction();
    test_image_extraction();
    test_link_extraction();
    test_inline_css();
    test_title_extraction();
    test_mixed_content();
    test_url_resolution();
    test_type_guessing();
    
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
