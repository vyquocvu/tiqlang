#!/bin/sh
# M18.5: Publisher tooling test harness.
# Tests: tiq publish, tiq yank, error cases.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TIQ="${TIQ:-./build/tiq}"
# Make TIQ path absolute for use after cd
if [ "${TIQ#/}" = "$TIQ" ]; then
  TIQ="$(pwd)/$TIQ"
fi

TMP_DIR=$(mktemp -d /tmp/tiq-publish-test.XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

fail=0

# Build publish tool
PUBLISH="$ROOT/build/tiq-publish"
if ! "$TIQ" build src/tiq/tools/publish.tiq -o "$PUBLISH" 2>"$TMP_DIR/build_pub.err"; then
  echo "publish_tool: FAIL (cannot build src/tiq/tools/publish.tiq)" >&2
  cat "$TMP_DIR/build_pub.err" >&2
  exit 1
fi

# Build yank tool
YANK="$ROOT/build/tiq-yank"
if ! "$TIQ" build src/tiq/tools/yank.tiq -o "$YANK" 2>"$TMP_DIR/build_yank.err"; then
  echo "publish_tool: FAIL (cannot build src/tiq/tools/yank.tiq)" >&2
  cat "$TMP_DIR/build_yank.err" >&2
  exit 1
fi

# Build registry server
REGISTRY="$ROOT/build/tiq-registry"
if ! "$TIQ" build src/tiq/tools/registry.tiq -o "$REGISTRY" 2>"$TMP_DIR/build_reg.err"; then
  echo "publish_tool: FAIL (cannot build src/tiq/tools/registry.tiq)" >&2
  cat "$TMP_DIR/build_reg.err" >&2
  exit 1
fi

# Build search tool for verification
SEARCH="$ROOT/build/tiq-search"
if ! "$TIQ" build src/tiq/tools/search.tiq -o "$SEARCH" 2>"$TMP_DIR/build_search.err"; then
  echo "publish_tool: FAIL (cannot build src/tiq/tools/search.tiq)" >&2
  cat "$TMP_DIR/build_search.err" >&2
  exit 1
fi

# Clean registry state
rm -rf /tmp/.tiq-registry

# Start registry server
PORT=19092
"$REGISTRY" "$PORT" &
REG_PID=$!
sleep 1

cleanup() {
  kill "$REG_PID" 2>/dev/null || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# 1. Publish a package
echo "=== 1. Publish ==="
mkdir -p "$TMP_DIR/pkg1"
cd "$TMP_DIR/pkg1"
cat > tiq.toml << 'EOF'
[package]
name = "mylib"
version = "1.0.0"
repository = "https://github.com/test/mylib.git"

[tests]
dir = "tests"
EOF
result=$("$PUBLISH" --registry "http://127.0.0.1:$PORT" 2>&1)
case "$result" in
  *"Published mylib 1.0.0"*) ;;
  *) echo "publish_tool: FAIL publish (got: $result)" >&2; fail=1 ;;
esac

# 2. Verify via registry API
echo "=== 2. Verify published ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/mylib")
case "$result" in
  *'"mylib"'*'"1.0.0"'*) ;;
  *) echo "publish_tool: FAIL verify (got: $result)" >&2; fail=1 ;;
esac

# 3. Publish second version
echo "=== 3. Publish v2 ==="
mkdir -p "$TMP_DIR/pkg2"
cd "$TMP_DIR/pkg2"
cat > tiq.toml << 'EOF'
[package]
name = "mylib"
version = "1.1.0"
repository = "https://github.com/test/mylib.git"

[tests]
dir = "tests"
EOF
result=$("$PUBLISH" --registry "http://127.0.0.1:$PORT" 2>&1)
case "$result" in
  *"Published mylib 1.1.0"*) ;;
  *) echo "publish_tool: FAIL publish v2 (got: $result)" >&2; fail=1 ;;
esac

# 4. Duplicate publish fails
echo "=== 4. Duplicate publish ==="
cd "$TMP_DIR/pkg2"
set +e
result=$("$PUBLISH" --registry "http://127.0.0.1:$PORT" 2>&1)
rc=$?
set -e
if [ $rc -eq 0 ]; then
  echo "publish_tool: FAIL duplicate should exit non-zero" >&2
  fail=1
fi
case "$result" in
  *"already exists"*) ;;
  *) echo "publish_tool: FAIL duplicate msg (got: $result)" >&2; fail=1 ;;
esac

# 5. Yank a version
echo "=== 5. Yank ==="
result=$("$YANK" --registry "http://127.0.0.1:$PORT" mylib 1.0.0 2>&1)
case "$result" in
  *"Yanked mylib 1.0.0"*) ;;
  *) echo "publish_tool: FAIL yank (got: $result)" >&2; fail=1 ;;
esac

# 6. Verify yanked version is gone
echo "=== 6. Verify yank ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/mylib")
case "$result" in
  *'"1.1.0"'*) ;;
  *) echo "publish_tool: FAIL verify yank (got: $result)" >&2; fail=1 ;;
esac
case "$result" in
  *'"1.0.0"'*) echo "publish_tool: FAIL yanked version still present (got: $result)" >&2; fail=1 ;;
  *) ;;
esac

# 7. Yank nonexistent version
echo "=== 7. Yank nonexistent ==="
set +e
result=$("$YANK" --registry "http://127.0.0.1:$PORT" mylib 9.9.9 2>&1)
rc=$?
set -e
if [ $rc -eq 0 ]; then
  echo "publish_tool: FAIL yank nonexistent should exit non-zero" >&2
  fail=1
fi
case "$result" in
  *"not found"*) ;;
  *) echo "publish_tool: FAIL yank nonexistent msg (got: $result)" >&2; fail=1 ;;
esac

# 8. Yank missing args
echo "=== 8. Yank missing args ==="
set +e
result=$("$YANK" --registry "http://127.0.0.1:$PORT" 2>&1)
rc=$?
set -e
if [ $rc -ne 2 ]; then
  echo "publish_tool: FAIL yank no args should exit 2 (got rc=$rc)" >&2
  fail=1
fi

# 9. Publish with no manifest
echo "=== 9. Publish no manifest ==="
mkdir -p "$TMP_DIR/empty"
cd "$TMP_DIR/empty"
set +e
result=$("$PUBLISH" --registry "http://127.0.0.1:$PORT" 2>&1)
rc=$?
set -e
if [ $rc -ne 1 ]; then
  echo "publish_tool: FAIL no manifest should exit 1 (got rc=$rc)" >&2
  fail=1
fi

# 10. Publish with no version
echo "=== 10. Publish no version ==="
mkdir -p "$TMP_DIR/nover"
cd "$TMP_DIR/nover"
cat > tiq.toml << 'EOF'
[package]
name = "noversion"

[tests]
dir = "tests"
EOF
set +e
result=$("$PUBLISH" --registry "http://127.0.0.1:$PORT" 2>&1)
rc=$?
set -e
if [ $rc -ne 1 ]; then
  echo "publish_tool: FAIL no version should exit 1 (got rc=$rc)" >&2
  fail=1
fi

# 11. Publish unknown option
echo "=== 11. Publish unknown option ==="
cd "$TMP_DIR/pkg1"
set +e
result=$("$PUBLISH" --bogus 2>&1)
rc=$?
set -e
if [ $rc -ne 2 ]; then
  echo "publish_tool: FAIL unknown option should exit 2 (got rc=$rc)" >&2
  fail=1
fi

# 12. Publish with default source (no repository field)
echo "=== 12. Publish default source ==="
mkdir -p "$TMP_DIR/norepo"
cd "$TMP_DIR/norepo"
cat > tiq.toml << 'EOF'
[package]
name = "norepo"
version = "0.1.0"

[tests]
dir = "tests"
EOF
result=$("$PUBLISH" --registry "http://127.0.0.1:$PORT" 2>&1)
case "$result" in
  *"Published norepo 0.1.0"*) ;;
  *) echo "publish_tool: FAIL default source (got: $result)" >&2; fail=1 ;;
esac
# Verify source is path:.
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/norepo/0.1.0")
case "$result" in
  *'"path:."'*) ;;
  *) echo "publish_tool: FAIL default source should be path:. (got: $result)" >&2; fail=1 ;;
esac

# 13. Search shows published packages
echo "=== 13. Search shows published ==="
result=$("$SEARCH" --registry "http://127.0.0.1:$PORT" 2>&1) || true
case "$result" in
  *"mylib"*) ;;
  *) echo "publish_tool: FAIL search after publish (got: $result)" >&2; fail=1 ;;
esac

# Kill server
kill "$REG_PID" 2>/dev/null || true
wait "$REG_PID" 2>/dev/null || true

if [ "$fail" -ne 0 ]; then
  echo "publish_tool: FAIL" >&2
  exit 1
fi

echo "publish_tool: ok (publish, yank, error cases)"
