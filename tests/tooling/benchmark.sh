#!/bin/sh
# Test the Tiq benchmark command

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-bench-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "=== Testing benchmark ==="

# Create test files (valid Tiq code only)
cat > "$TMP/small.tiq" << 'EOF'
! "Hello"
EOF

cat > "$TMP/medium.tiq" << 'EOF'
fib = [0, 1, ... a + b]
[0..10 | !fib[i]]
EOF

# Test benchmark on single file
$TIQ bench "$TMP/small.tiq"
echo "bench single file: passed"

# Test benchmark on multiple files
$TIQ bench "$TMP/small.tiq" "$TMP/medium.tiq"
echo "bench multiple files: passed"

# Test benchmark on directory
$TIQ bench "$TMP"
echo "bench directory: passed"

# Directory results must print real file names (regression: names were
# freed before printing, producing garbage output)
$TIQ bench "$TMP" | grep -q "small.tiq"
echo "bench directory names: passed"

# Test benchmark verbose mode
$TIQ bench -v "$TMP/small.tiq" 2>&1 | head -20
echo "bench verbose: passed"

# Test benchmark quiet mode
$TIQ bench -q "$TMP"
echo "bench quiet: passed"

# Test benchmark with iterations
$TIQ bench -i 5 "$TMP/small.tiq"
echo "bench with iterations: passed"

# Test benchmark on examples directory
$TIQ bench -q examples/
echo "bench examples: passed"

echo "benchmark tests: ok"
