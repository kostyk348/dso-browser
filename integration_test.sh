#!/bin/bash
# integration_test.sh — Full pipeline test for PSIRP stack

BASEDIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$BASEDIR/build"
CONTENT_DIR="$BASEDIR/content"
PASS=0
FAIL=0
PIDS=""

cleanup() {
    for pid in $PIDS; do
        kill $pid 2>/dev/null
        wait $pid 2>/dev/null
    done
}
trap cleanup EXIT

echo "=== PSIRP Integration Test ==="
echo ""

# Build
echo "Building..."
cd "$BUILD_DIR" && cmake "$BASEDIR" -DCMAKE_BUILD_TYPE=Release 2>/dev/null
make -j$(nproc) 2>/dev/null
cd "$BASEDIR"
echo "Build complete"
echo ""

# Test 1: Publisher + Client
echo "--- Test 1: Publisher + Client ---"
$BUILD_DIR/publisher 9700 "$CONTENT_DIR" &
PIDS="$PIDS $!"
sleep 2

OUTPUT=$(timeout 10 $BUILD_DIR/client /test/page.html 127.0.0.1 9700 2>&1)
if echo "$OUTPUT" | grep -q "Found"; then
    echo "[PASS] Publisher serves content, client fetches by name"
    PASS=$((PASS+1))
else
    echo "[FAIL] Publisher/Client exchange"
    FAIL=$((FAIL+1))
fi

# Kill publisher
for pid in $PIDS; do kill $pid 2>/dev/null; done
PIDS=""
sleep 1
echo ""

# Test 2: Content Store
echo "--- Test 2: Content Store ---"
OUTPUT=$($BUILD_DIR/test_psirp 2>&1)
if echo "$OUTPUT" | grep -q "10/10 passed"; then
    echo "[PASS] Content store tests (10/10)"
    PASS=$((PASS+1))
else
    echo "[FAIL] Content store tests"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 3: HTML Parser
echo "--- Test 3: HTML Parser ---"
OUTPUT=$($BUILD_DIR/test_html_parse 2>&1)
if echo "$OUTPUT" | grep -q "9 passed, 0 failed"; then
    echo "[PASS] HTML parser tests (9/9)"
    PASS=$((PASS+1))
else
    echo "[FAIL] HTML parser tests"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 4: Mesh
echo "--- Test 4: Mesh ---"
OUTPUT=$($BUILD_DIR/test_mesh 2>&1)
if echo "$OUTPUT" | grep -q "7 passed, 0 failed"; then
    echo "[PASS] Mesh tests (7/7)"
    PASS=$((PASS+1))
else
    echo "[FAIL] Mesh tests"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 4b: DHT
echo "--- Test 4b: DHT + Ed25519 ---"
OUTPUT=$($BUILD_DIR/test_dht 2>&1)
if echo "$OUTPUT" | grep -q "10 passed"; then
    echo "[PASS] DHT + Ed25519 tests (10/10)"
    PASS=$((PASS+1))
else
    echo "[FAIL] DHT + Ed25519 tests"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 4c: Dynamic content (versioning, chunking, LRU, pub/sub, compute)
echo "--- Test 4c: Dynamic Content ---"
OUTPUT=$($BUILD_DIR/test_dynamic 2>&1)
if echo "$OUTPUT" | grep -q "8 passed"; then
    echo "[PASS] Dynamic content tests (8/8)"
    PASS=$((PASS+1))
else
    echo "[FAIL] Dynamic content tests"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 5: Signing
echo "--- Test 5: Signing ---"
OUTPUT=$($BUILD_DIR/test_signing 2>&1)
if echo "$OUTPUT" | grep -q "5 passed, 0 failed"; then
    echo "[PASS] Signing tests (5/5)"
    PASS=$((PASS+1))
else
    echo "[FAIL] Signing tests"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 6: Terminal Browser
echo "--- Test 6: Terminal Browser ---"
if [ -f "$BUILD_DIR/terminal_browser" ]; then
    echo "[PASS] Terminal browser built"
    PASS=$((PASS+1))
else
    echo "[FAIL] Terminal browser not built"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 7: Mesh Demo
echo "--- Test 7: Mesh Demo ---"
if [ -f "$BUILD_DIR/mesh_demo" ]; then
    echo "[PASS] Mesh demo built"
    PASS=$((PASS+1))
else
    echo "[FAIL] Mesh demo not built"
    FAIL=$((FAIL+1))
fi
echo ""

# Test 8: Multiple files
echo "--- Test 8: Multiple Files ---"
$BUILD_DIR/publisher 9701 "$CONTENT_DIR" &
PIDS="$PIDS $!"
sleep 2

OUTPUT1=$(timeout 10 $BUILD_DIR/client /test/page.html 127.0.0.1 9701 2>&1)
OUTPUT2=$(timeout 10 $BUILD_DIR/client /test/page2.html 127.0.0.1 9701 2>&1)

if echo "$OUTPUT1" | grep -q "Found" && echo "$OUTPUT2" | grep -q "Found"; then
    echo "[PASS] Publisher serves multiple files"
    PASS=$((PASS+1))
else
    echo "[FAIL] Multiple file serving"
    FAIL=$((FAIL+1))
fi

for pid in $PIDS; do kill $pid 2>/dev/null; done
PIDS=""
sleep 1
echo ""

# Test 9: Content integrity
echo "--- Test 9: Content Integrity ---"
$BUILD_DIR/publisher 9702 "$CONTENT_DIR" &
PIDS="$PIDS $!"
sleep 2

OUTPUT=$(timeout 10 $BUILD_DIR/client /test/page.html 127.0.0.1 9702 2>&1)
if echo "$OUTPUT" | grep -q "DSO-PSIRP Test Page"; then
    echo "[PASS] Content integrity verified"
    PASS=$((PASS+1))
else
    echo "[FAIL] Content integrity"
    FAIL=$((FAIL+1))
fi

for pid in $PIDS; do kill $pid 2>/dev/null; done
PIDS=""
sleep 1
echo ""

# Test 10: All binaries
echo "--- Test 10: All Binaries ---"
BINARIES="publisher client terminal_browser mesh_demo dso-browser test_psirp test_html_parse test_mesh test_signing"
ALL_FOUND=1
for bin in $BINARIES; do
    if [ ! -f "$BUILD_DIR/$bin" ]; then
        echo "[FAIL] Missing: $bin"
        ALL_FOUND=0
    fi
done
if [ $ALL_FOUND -eq 1 ]; then
    echo "[PASS] All 9 binaries built"
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
fi
echo ""

# Summary
echo "=== Integration Test Results ==="
echo "Passed: $PASS / 12"
echo "Failed: $FAIL / 12"
echo ""
echo "Unit tests: PSIRP(10) + HTML(9) + Mesh(7) + Signing(5) + DHT(10) + Dynamic(8) = 49 total"
echo ""

if [ $FAIL -eq 0 ]; then
    echo "ALL TESTS PASSED!"
    exit 0
else
    echo "Some tests failed"
    exit 1
fi
