/**
 * @file publisher.cpp
 * @brief PSIRP Content Publisher — Serves content to browsers.
 *
 * Usage:
 *   ./publisher <port> [content_dir]
 *
 * Listens for Interest packets and serves content from local files.
 * Content names map to file paths:
 *   /site/page → content_dir/site/page.html
 *   /images/logo.png → content_dir/images/logo.png
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>

extern "C" {
#include "psirp.h"
#include "net.h"
}

namespace fs = std::filesystem;

// ── Content Store ─────────────────────────────────────────────────────────────

static uint8_t cs_arena_mem[16 * 1024 * 1024]; // 16MB content store
static psirp_cs cs;
static psirp_node net;
static std::string content_dir = ".";

// ── File Loading ──────────────────────────────────────────────────────────────

static std::string load_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return content;
}

static void publish_file(const std::string &name, const std::string &file_path) {
    std::string content = load_file(file_path);
    if (content.empty()) {
        fprintf(stderr, "Failed to load: %s\n", file_path.c_str());
        return;
    }
    
    psirp_name psirp_name;
    if (!psirp_name_init(&psirp_name, name.c_str())) {
        fprintf(stderr, "Invalid name: %s\n", name.c_str());
        return;
    }
    
    // Detect freshness from extension
    uint32_t freshness = 0; // forever
    if (file_path.find(".html") != std::string::npos) {
        freshness = 60000; // 1 minute for HTML
    } else if (file_path.find(".css") != std::string::npos) {
        freshness = 300000; // 5 minutes for CSS
    } else if (file_path.find(".js") != std::string::npos) {
        freshness = 300000; // 5 minutes for JS
    }
    
    if (psirp_net_publish(&net, &psirp_name, (const uint8_t *)content.data(),
                          content.size(), freshness)) {
        printf("Published: %s (%zu bytes)\n", name.c_str(), content.size());
    } else {
        fprintf(stderr, "Failed to publish: %s\n", name.c_str());
    }
}

static void publish_directory(const std::string &dir, const std::string &prefix) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        fprintf(stderr, "Directory not found: %s\n", dir.c_str());
        return;
    }
    
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        
        // Convert file path to content name
        std::string rel = fs::relative(entry.path(), dir).string();
        std::string name = prefix + "/" + rel;
        
        // Remove file extension for cleaner URLs
        size_t dot = name.rfind('.');
        if (dot != std::string::npos) {
            // Keep extension for now (helps with content type detection)
        }
        
        publish_file(name, entry.path().string());
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    uint16_t port = PSIRP_DEFAULT_PORT;
    
    if (argc > 1) {
        port = (uint16_t)atoi(argv[1]);
    }
    
    if (argc > 2) {
        content_dir = argv[2];
    }
    
    printf("DSO-PSIRP Publisher\n");
    printf("Port: %u\n", port);
    printf("Content directory: %s\n", content_dir.c_str());
    printf("\n");
    
    // Initialize content store
    psirp_cs_init(&cs, cs_arena_mem, sizeof(cs_arena_mem));
    
    // Initialize network
    if (!psirp_net_init(&net, port, &cs)) {
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }
    
    // Start receiver
    if (!psirp_net_start(&net)) {
        fprintf(stderr, "Failed to start receiver\n");
        return 1;
    }
    
    // Publish content
    printf("Publishing content...\n");
    
    // Publish a test page
    const char *test_page = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>Test Page</title></head>\n"
        "<body>\n"
        "<h1>DSO-PSIRP Test Page</h1>\n"
        "<p>This content was served via PSIRP content-centric networking.</p>\n"
        "<p>Content name: <code>/test/page</code></p>\n"
        "<hr>\n"
        "<h2>Links:</h2>\n"
        "<ul>\n"
        "<li><a href='/test/page2'>Page 2</a></li>\n"
        "<li><a href='/images/logo.png'>Logo (PNG)</a></li>\n"
        "</ul>\n"
        "</body>\n"
        "</html>";
    
    psirp_name test_name;
    psirp_name_init(&test_name, "/test/page");
    psirp_net_publish(&net, &test_name, (const uint8_t *)test_page, strlen(test_page), 60000);
    printf("Published: /test/page (%zu bytes)\n", strlen(test_page));
    
    // Publish second test page
    const char *test_page2 = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>Test Page 2</title></head>\n"
        "<body>\n"
        "<h1>Test Page 2</h1>\n"
        "<p>Another page served via PSIRP.</p>\n"
        "<p><a href='/test/page'>Back to Page 1</a></p>\n"
        "</body>\n"
        "</html>";
    
    psirp_name test_name2;
    psirp_name_init(&test_name2, "/test/page2");
    psirp_net_publish(&net, &test_name2, (const uint8_t *)test_page2, strlen(test_page2), 60000);
    printf("Published: /test/page2 (%zu bytes)\n", strlen(test_page2));
    
    // Publish from directory if provided
    if (fs::exists(content_dir) && fs::is_directory(content_dir)) {
        publish_directory(content_dir, "");
    }
    
    printf("\nPublisher ready. Waiting for interests...\n");
    printf("Press Ctrl+C to stop.\n\n");
    
    // Main loop
    while (true) {
        // GC expired content
        psirp_cs_gc(&cs);
        
        // Sleep 1 second
        struct timespec ts = { .tv_sec = 1 };
        nanosleep(&ts, NULL);
    }
    
    // Cleanup (never reached)
    psirp_net_stop(&net);
    
    return 0;
}
