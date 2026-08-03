#!/bin/sh
# M18.4: Registry server and search tool test harness.
# Tests: registry API (list, publish, get, yank), search tool, registry deps.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TIQ="${TIQ:-./build/tiq}"
# Make TIQ path absolute for use after cd
if [ "${TIQ#/}" = "$TIQ" ]; then
  TIQ="$(pwd)/$TIQ"
fi

TMP_DIR=$(mktemp -d /tmp/tiq-registry-test.XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

fail=0

# Build registry server
REGISTRY="$ROOT/build/tiq-registry"
if ! "$TIQ" build src/tiq/tools/registry.tiq -o "$REGISTRY" 2>"$TMP_DIR/build_reg.err"; then
  echo "registry_tool: FAIL (cannot build src/tiq/tools/registry.tiq)" >&2
  cat "$TMP_DIR/build_reg.err" >&2
  exit 1
fi

# Build search tool
SEARCH="$ROOT/build/tiq-search"
if ! "$TIQ" build src/tiq/tools/search.tiq -o "$SEARCH" 2>"$TMP_DIR/build_search.err"; then
  echo "registry_tool: FAIL (cannot build src/tiq/tools/search.tiq)" >&2
  cat "$TMP_DIR/build_search.err" >&2
  exit 1
fi

# Build init tool for manifest validation
INIT="$ROOT/build/tiq-init"
if ! "$TIQ" build src/tiq/tools/init.tiq -o "$INIT" 2>"$TMP_DIR/build_init.err"; then
  echo "registry_tool: FAIL (cannot build src/tiq/tools/init.tiq)" >&2
  cat "$TMP_DIR/build_init.err" >&2
  exit 1
fi

# Clean registry state
rm -rf /tmp/.tiq-registry

# Start registry server on ephemeral-ish port
PORT=19090
"$REGISTRY" "$PORT" &
REG_PID=$!
sleep 1

# Ensure server is killed on exit
cleanup() {
  kill "$REG_PID" 2>/dev/null || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# 1. List empty registry
echo "=== 1. List empty ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages")
if [ "$result" != "[]" ]; then
  echo "registry_tool: FAIL list empty (got: $result)" >&2
  fail=1
fi

# 2. Publish a package
echo "=== 2. Publish ==="
result=$(curl -s -X PUT -d '{"source":"git:https://example.com/mylib.git"}' "http://127.0.0.1:$PORT/api/v1/packages/mylib/1.0.0")
case "$result" in
  *'"published"'*) ;;
  *) echo "registry_tool: FAIL publish (got: $result)" >&2; fail=1 ;;
esac

# 3. Get package metadata
echo "=== 3. Get package ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/mylib")
case "$result" in
  *'"mylib"'*'"versions"'*) ;;
  *) echo "registry_tool: FAIL get package (got: $result)" >&2; fail=1 ;;
esac

# 4. Get specific version
echo "=== 4. Get version ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/mylib/1.0.0")
case "$result" in
  *'"1.0.0"'*'"source"'*) ;;
  *) echo "registry_tool: FAIL get version (got: $result)" >&2; fail=1 ;;
esac

# 5. List with package
echo "=== 5. List with package ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages")
case "$result" in
  *'"mylib"'*) ;;
  *) echo "registry_tool: FAIL list with package (got: $result)" >&2; fail=1 ;;
esac

# 6. Publish second version
echo "=== 6. Publish v2 ==="
curl -s -X PUT -d '{"source":"git:https://example.com/mylib.git"}' "http://127.0.0.1:$PORT/api/v1/packages/mylib/1.1.0" > /dev/null

# 7. Get package (should have 2 versions)
echo "=== 7. Get package 2 versions ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/mylib")
case "$result" in
  *'"1.0.0"'*'"1.1.0"'*) ;;
  *) echo "registry_tool: FAIL 2 versions (got: $result)" >&2; fail=1 ;;
esac

# 8. Yank a version
echo "=== 8. Yank ==="
result=$(curl -s -X DELETE "http://127.0.0.1:$PORT/api/v1/packages/mylib/1.0.0")
case "$result" in
  *'"yanked"'*) ;;
  *) echo "registry_tool: FAIL yank (got: $result)" >&2; fail=1 ;;
esac

# 9. Get after yank
echo "=== 9. Get after yank ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/mylib")
case "$result" in
  *'"1.1.0"'*) ;;
  *) echo "registry_tool: FAIL after yank (got: $result)" >&2; fail=1 ;;
esac

# 10. 404 for nonexistent package
echo "=== 10. 404 ==="
result=$(curl -s "http://127.0.0.1:$PORT/api/v1/packages/nonexistent")
case "$result" in
  *'"error"'*) ;;
  *) echo "registry_tool: FAIL 404 (got: $result)" >&2; fail=1 ;;
esac

# 11. Duplicate publish fails
echo "=== 11. Duplicate publish ==="
result=$(curl -s -X PUT -d '{"source":"git:https://example.com/mylib.git"}' "http://127.0.0.1:$PORT/api/v1/packages/mylib/1.1.0")
case "$result" in
  *'"error"'*) ;;
  *) echo "registry_tool: FAIL duplicate publish (got: $result)" >&2; fail=1 ;;
esac

# 12. Search tool - list all
echo "=== 12. Search all ==="
result=$("$SEARCH" --registry "http://127.0.0.1:$PORT" 2>&1) || true
case "$result" in
  *"mylib"*) ;;
  *) echo "registry_tool: FAIL search all (got: $result)" >&2; fail=1 ;;
esac

# 13. Search tool - filter
echo "=== 13. Search filter ==="
result=$("$SEARCH" --registry "http://127.0.0.1:$PORT" "my" 2>&1) || true
case "$result" in
  *"mylib"*) ;;
  *) echo "registry_tool: FAIL search filter (got: $result)" >&2; fail=1 ;;
esac

# 14. Search tool - no match
echo "=== 14. Search no match ==="
set +e
result=$("$SEARCH" --registry "http://127.0.0.1:$PORT" "zzz" 2>&1)
rc=$?
set -e
if [ $rc -eq 0 ]; then
  echo "registry_tool: FAIL search no match (expected exit 1)" >&2
  fail=1
fi

# 15. Manifest validation: registry: scheme
echo "=== 15. Manifest registry scheme ==="
mkdir -p "$TMP_DIR/reg_manifest"
cd "$TMP_DIR/reg_manifest"
cat > "tiq.toml" << 'EOF'
[package]
name = "regapp"

[deps]
mylib = "registry:mylib"
other = "registry:other#>=1.0.0,<2.0.0"
EOF
"$INIT" --check tiq.toml >"$TMP_DIR/reg_check.out" 2>"$TMP_DIR/reg_check.err"
if [ $? -ne 0 ]; then
  echo "registry_tool: FAIL registry scheme manifest validation" >&2
  cat "$TMP_DIR/reg_check.err" >&2
  fail=1
fi

# 16. Manifest validation: invalid registry name
echo "=== 16. Invalid registry name ==="
mkdir -p "$TMP_DIR/reg_bad"
cd "$TMP_DIR/reg_bad"
cat > "tiq.toml" << 'EOF'
[package]
name = "badreg"

[deps]
foo = "registry:"
EOF
"$INIT" --check tiq.toml >"$TMP_DIR/reg_bad.out" 2>"$TMP_DIR/reg_bad.err" || true
if ! grep -q "must not be empty\|invalid registry" "$TMP_DIR/reg_bad.err" 2>/dev/null; then
  echo "registry_tool: FAIL invalid registry name not rejected" >&2
  fail=1
fi

# Kill server
kill "$REG_PID" 2>/dev/null || true
wait "$REG_PID" 2>/dev/null || true

if [ "$fail" -ne 0 ]; then
  echo "registry_tool: FAIL" >&2
  exit 1
fi

echo "registry_tool: ok (API endpoints, search, manifest validation)"
