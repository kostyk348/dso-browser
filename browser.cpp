/**
 * @file browser.cpp
 * @brief DSO-PSIRP Browser — Content-centric web browser.
 *
 * Features:
 * - Address bar accepts content names (/site/page/resource)
 * - Deterministic rendering via DSO task graphs
 * - Content-centric networking via PSIRP
 * - In-memory content store (arena-backed)
 *
 * Built with GTK4 + WebKitGTK 6.0
 */

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

extern "C" {
#include "psirp.h"
#include "net.h"
#include "dso_psirp.h"
}

// ── Global State ──────────────────────────────────────────────────────────────

struct BrowserApp {
    // GTK4 application
    GtkApplication *gtk_app;
    GtkWidget      *window;
    GtkWidget      *header;
    GtkWidget      *url_entry;
    GtkWidget      *web_view;

    // PSIRP state
    psirp_cs      cs;
    psirp_node    net;
    dso_psirp_ctx fetch_ctx;

    // Memory
    uint8_t cs_arena_mem[4 * 1024 * 1024];  // 4MB content store
    uint8_t task_arena_mem[1 * 1024 * 1024]; // 1MB task arena
    dso_arena task_arena;

    // Current content name
    char current_name[PSIRP_MAX_NAME];
};

static BrowserApp app;

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool is_content_name(const char *text) {
    return text && text[0] == '/';
}

static bool is_url(const char *text) {
    return text && (strstr(text, "://") != NULL);
}

// ── Navigation ────────────────────────────────────────────────────────────────

static void navigate_to(const char *name) {
    if (!name || !name[0]) return;

    // Update address bar
    gtk_editable_set_text(GTK_EDITABLE(app.url_entry), name);

    // Save current name
    strncpy(app.current_name, name, PSIRP_MAX_NAME - 1);

    // Parse content name
    psirp_name content_name;
    if (!psirp_name_init(&content_name, name)) {
        return;
    }

    // Fetch content via PSIRP
    const psirp_cs_entry *entry = psirp_net_fetch(&app.net, &content_name,
                                                   DSO_PSIRP_FETCH_TIMEOUT);

    if (!entry) {
        // Show not-found page
        char not_found[512];
        snprintf(not_found, sizeof(not_found),
            "<html><body style='font-family: sans-serif; padding: 40px;'>"
            "<h1>404 — Content Not Found</h1>"
            "<p>Content name: <code>%s</code></p>"
            "<p>The requested content is not available in the content store.</p>"
            "</body></html>", name);
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(app.web_view), not_found, NULL);
        return;
    }

    // Load content into WebKit
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(app.web_view),
                              (const char *)entry->data,
                              NULL);
}

// ── Callbacks ─────────────────────────────────────────────────────────────────

static void on_url_activate(GtkEntry *entry, gpointer user_data) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (is_content_name(text)) {
        navigate_to(text);
    } else if (is_url(text)) {
        // Convert URL to content name: http://example.com/page → /example.com/page
        const char *path = strstr(text, "://");
        if (path) {
            path += 3;
            char name[PSIRP_MAX_NAME];
            snprintf(name, sizeof(name), "/%s", path);
            navigate_to(name);
        }
    } else {
        navigate_to(text);
    }
}

static void on_back_clicked(GtkButton *button, gpointer user_data) {
    webkit_web_view_go_back(WEBKIT_WEB_VIEW(app.web_view));
}

static void on_forward_clicked(GtkButton *button, gpointer user_data) {
    webkit_web_view_go_forward(WEBKIT_WEB_VIEW(app.web_view));
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    if (app.current_name[0]) {
        navigate_to(app.current_name);
    }
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event,
                            gpointer user_data) {
    if (event != WEBKIT_LOAD_COMMITTED) return;

    const char *uri = webkit_web_view_get_uri(web_view);
    if (uri && uri[0]) {
        gtk_editable_set_text(GTK_EDITABLE(app.url_entry), uri);
    }
}

// ── GTK4 Application ──────────────────────────────────────────────────────────

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
    // Window
    app.window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app.window), "DSO Browser — Content-Centric");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1280, 720);

    // Header bar
    app.header = gtk_header_bar_new();

    // Back button
    GtkWidget *back_button = gtk_button_new_from_icon_name("go-previous");
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(app.header), back_button);

    // Forward button
    GtkWidget *forward_button = gtk_button_new_from_icon_name("go-next");
    g_signal_connect(forward_button, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(app.header), forward_button);

    // Refresh button
    GtkWidget *refresh_button = gtk_button_new_from_icon_name("view-refresh");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(app.header), refresh_button);

    // URL entry
    app.url_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app.url_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.url_entry),
        "Enter content name: /site/page/resource");
    g_signal_connect(app.url_entry, "activate", G_CALLBACK(on_url_activate), NULL);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(app.header), app.url_entry);

    gtk_window_set_titlebar(GTK_WINDOW(app.window), app.header);

    // Web view
    app.web_view = webkit_web_view_new();

    WebKitSettings *settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(app.web_view));
    webkit_settings_set_enable_javascript(settings, TRUE);

    g_signal_connect(app.web_view, "load-changed",
                    G_CALLBACK(on_load_changed), NULL);

    gtk_window_set_child(GTK_WINDOW(app.window), app.web_view);

    // Welcome page
    const char *welcome =
        "<html><body style='font-family: sans-serif; padding: 40px;'>"
        "<h1>DSO Browser</h1>"
        "<p>Content-centric web browser powered by PSIRP + DSO.</p>"
        "<hr>"
        "<h2>How to use:</h2>"
        "<ul>"
        "<li>Enter content names in the address bar: <code>/site/page</code></li>"
        "<li>Content is addressed by name, not by IP</li>"
        "<li>Fetching is deterministic via DSO task graphs</li>"
        "</ul>"
        "<hr>"
        "<h2>Try:</h2>"
        "<ul>"
        "<li><a href='/test/page'>Test page</a></li>"
        "<li><a href='/test/page2'>Page 2</a></li>"
        "</ul>"
        "</body></html>";

    webkit_web_view_load_html(WEBKIT_WEB_VIEW(app.web_view), welcome, NULL);

    gtk_window_present(GTK_WINDOW(app.window));
}

// ── PSIRP Init ────────────────────────────────────────────────────────────────

static bool init_psirp(void) {
    psirp_cs_init(&app.cs, app.cs_arena_mem, sizeof(app.cs_arena_mem));

    if (!psirp_net_init(&app.net, PSIRP_DEFAULT_PORT, &app.cs)) {
        fprintf(stderr, "Failed to initialize PSIRP network\n");
        return false;
    }

    psirp_net_add_peer(&app.net, inet_addr("127.0.0.1"), PSIRP_DEFAULT_PORT);

    dso_arena_init(&app.task_arena, app.task_arena_mem, sizeof(app.task_arena_mem));
    dso_psirp_ctx_init(&app.fetch_ctx, &app.net, &app.cs, &app.task_arena);

    if (!psirp_net_start(&app.net)) {
        fprintf(stderr, "Failed to start PSIRP receiver\n");
        return false;
    }

    return true;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    // Initialize PSIRP before GTK
    if (!init_psirp()) {
        return 1;
    }

    // Create GTK4 application
    app.gtk_app = gtk_application_new("org.dso.browser", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.gtk_app, "activate", G_CALLBACK(on_activate), NULL);

    // Run
    int status = g_application_run(G_APPLICATION(app.gtk_app), argc, argv);

    // Cleanup
    psirp_net_stop(&app.net);
    g_object_unref(app.gtk_app);

    return status;
}
