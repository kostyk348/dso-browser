cmake_minimum_required(VERSION 3.20)
project(dso-browser C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# WebKitGTK 6.0 (GTK4)
pkg_check_modules(WEBKIT REQUIRED webkitgtk-6.0)
pkg_check_modules(GTK4 REQUIRED gtk4)

# ── DSO Runtime (external) ──────────────────────────────────────────────────────

set(DSO_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dso-runtime")

add_library(dso_runtime STATIC IMPORTED)
set_target_properties(dso_runtime PROPERTIES
    IMPORTED_LOCATION "${DSO_RUNTIME_DIR}/build/libdso.a"
    INTERFACE_INCLUDE_DIRECTORIES "${DSO_RUNTIME_DIR}/include"
)

# ── PSIRP Library ──────────────────────────────────────────────────────────────

add_library(psirp STATIC
    psirp.c
    net.c
    dso_psirp.c
    html_parse.c
    mesh.c
)

target_include_directories(psirp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${DSO_RUNTIME_DIR}/include
)

target_link_libraries(psirp PUBLIC
    dso_runtime
    Threads::Threads
    ${WEBKIT_LIBRARIES}
)

# ── DSO Browser ────────────────────────────────────────────────────────────────

add_executable(dso-browser
    browser.cpp
)

target_link_libraries(dso-browser PRIVATE
    psirp
    ${WEBKIT_LIBRARIES}
    ${GTK4_LIBRARIES}
    Threads::Threads
)

target_include_directories(dso-browser PRIVATE
    ${WEBKIT_INCLUDE_DIRS}
    ${GTK4_INCLUDE_DIRS}
)

target_compile_options(dso-browser PRIVATE
    ${WEBKIT_CFLAGS_OTHER}
    ${GTK4_CFLAGS_OTHER}
)

# ── Publisher ──────────────────────────────────────────────────────────────────

add_executable(publisher
    publisher.cpp
)

target_link_libraries(publisher PRIVATE
    psirp
    Threads::Threads
)

# ── Client ─────────────────────────────────────────────────────────────────────

add_executable(client
    client.cpp
)

target_link_libraries(client PRIVATE
    psirp
    Threads::Threads
)

# ── Tests ──────────────────────────────────────────────────────────────────────

add_executable(test_psirp test_psirp.c)
target_link_libraries(test_psirp PRIVATE psirp Threads::Threads)

add_executable(test_html_parse test_html_parse.c)
target_link_libraries(test_html_parse PRIVATE psirp Threads::Threads)

add_executable(test_mesh test_mesh.c)
target_link_libraries(test_mesh PRIVATE psirp Threads::Threads)

add_executable(mesh_demo mesh_demo.c)
target_link_libraries(mesh_demo PRIVATE psirp Threads::Threads)

# ── Install ────────────────────────────────────────────────────────────────────

install(TARGETS dso-browser publisher client
    RUNTIME DESTINATION bin
)

# ── Summary ────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "DSO-PSIRP Browser Build Configuration:")
message(STATUS "  Browser:   dso-browser (GTK4 + WebKitGTK)")
message(STATUS "  Publisher: publisher (content server)")
message(STATUS "  Client:    client (content fetcher)")
message(STATUS "  Test:      test_psirp")
message(STATUS "")
