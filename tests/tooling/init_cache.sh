#!/bin/sh
# Test the Tiq init and cache commands

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-init-cache-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "=== Testing init ==="

ORIG_DIR="$(pwd)"
cd "$TMP"

# Test init with name
"$TIQ" init mypackage
if [ -f "mypackage.tiq.toml" ]; then
    echo "init with name: passed"
else
    echo "init with name failed"
    ls -la
    exit 1
fi

# Check manifest content
grep -q 'name = "mypackage"' mypackage.tiq.toml
grep -q 'version = "0.1.0"' mypackage.tiq.toml
grep -q '\[package\]' mypackage.tiq.toml
grep -q '\[tests\]' mypackage.tiq.toml
echo "init manifest content: passed"

# Test init without name
rm mypackage.tiq.toml
"$TIQ" init
if [ -f "tiq.toml" ]; then
    echo "init without name: passed"
else
    echo "init without name failed"
    ls -la
    exit 1
fi

# Test init creates tests directory
grep -q 'dir = "tests"' tiq.toml
echo "init tests directory reference: passed"

cd "$ORIG_DIR"

echo ""
echo "=== Testing cache ==="

# Test cache path
CACHE_PATH=$("$TIQ" cache path)
if [ -n "$CACHE_PATH" ]; then
    echo "cache path: passed (path: $CACHE_PATH)"
else
    echo "cache path failed"
    exit 1
fi

# Test cache clear
"$TIQ" cache clear
echo "cache clear: passed"

# Test cache path contains tiq
echo "$CACHE_PATH" | grep -q tiq && echo "cache path contains tiq: passed"

echo "init and cache tests: ok"
