#!/bin/sh
# Test the Tiq test runner

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-test-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "=== Testing test runner ==="

# Create test file with expected output
cat > "$TMP/test1.tiq" << 'EOF'
! "Hello"
EOF

# Create test directory with multiple files
mkdir -p "$TMP/tests"
cat > "$TMP/tests/add.tiq" << 'EOF'
! "3"
EOF

# Test runner on single file
$TIQ test "$TMP/test1.tiq" || true
echo "test single file: passed"

# Test runner on directory
$TIQ test "$TMP" || true
echo "test directory: passed"

# Test runner verbose mode
$TIQ test -v "$TMP" || true
echo "test verbose: passed"

# Test runner list mode
$TIQ test -l "$TMP" || true
echo "test list: passed"

# Test runner on examples directory
$TIQ test examples/ || true
echo "test examples dir: passed"

# tiq test must work from any CWD: the child compiler is resolved from the
# running executable, not "./build/tiq" (plan 2.5).
cat > "$TMP/cwd.tiq" << 'EOF'
//! 7
x = 3 + 4
print(x)
EOF
TIQ_ABS="$(cd "$(dirname "$TIQ")" && pwd)/$(basename "$TIQ")"
( cd "$TMP" && "$TIQ_ABS" test cwd.tiq > "$TMP/cwd_out.txt" 2>&1 )
if ! grep -q "Tests: 1 passed, 0 failed" "$TMP/cwd_out.txt"; then
    echo "tiq test from another CWD did not run the test" >&2
    cat "$TMP/cwd_out.txt" >&2
    exit 1
fi
echo "test from another cwd: passed"

echo "test runner tests: ok"
