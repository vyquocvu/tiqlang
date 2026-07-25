#!/bin/sh
# Test the Tiq formatter

set -e

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-fmt-test-$$"
mkdir -p "$TMP"

echo "=== Testing formatter ==="

# Test basic formatting
echo '! "Hello"' > "$TMP/basic.tiq"
$TIQ fmt "$TMP/basic.tiq" > "$TMP/basic_out.tiq"

# Test formatter with stdin
echo 'x = 1' | $TIQ fmt > "$TMP/stdin_out.tiq"

# Test --check mode (should exit 0 for valid code)
echo '! "valid"' > "$TMP/valid.tiq"
if $TIQ fmt --check "$TMP/valid.tiq" 2>&1; then
    echo "check mode: passed"
else
    echo "check mode failed unexpectedly"
    exit 1
fi

# Test formatter preserves strings correctly
echo '!"test string"' > "$TMP/string.tiq"
$TIQ fmt "$TMP/string.tiq" > "$TMP/string_out.tiq"
if grep -q '"test string"' "$TMP/string_out.tiq"; then
    echo "formatter preserves strings: passed"
else
    echo "formatter preserves strings: FAILED"
    exit 1
fi

# Test formatter with loops
cat > "$TMP/loop.tiq" << 'EOF'
x <- 0
[0..10 | x += i]
!x
EOF
$TIQ fmt "$TMP/loop.tiq" > "$TMP/loop_out.tiq"
echo "formatter handles loops: passed"

# Test formatter with functions
cat > "$TMP/fn.tiq" << 'EOF'
add a b -> a + b
!add(1, 2)
EOF
$TIQ fmt "$TMP/fn.tiq" > "$TMP/fn_out.tiq"
echo "formatter handles functions: passed"

# Test formatter with stream generators
cat > "$TMP/stream.tiq" << 'EOF'
fib = [0, 1, ... a + b]
!fib[10]
EOF
$TIQ fmt "$TMP/stream.tiq" > "$TMP/stream_out.tiq"
echo "formatter handles streams: passed"

# Test --use-tabs option
$TIQ fmt --use-tabs "$TMP/basic.tiq" > "$TMP/tabs_out.tiq"
echo "formatter --use-tabs: passed"

# Test --indent-width option
$TIQ fmt --indent-width 2 "$TMP/fn.tiq" > "$TMP/indent2_out.tiq"
echo "formatter --indent-width: passed"

# Test --output option
$TIQ fmt "$TMP/basic.tiq" --output "$TMP/output.tiq"
if test -f "$TMP/output.tiq"; then
    echo "formatter --output: passed"
else
    echo "formatter --output: FAILED"
    exit 1
fi

# Test that formatted output is valid Tiq code
if $TIQ check "$TMP/basic_out.tiq" 2>/dev/null; then
    echo "formatter produces valid output: passed"
else
    echo "formatter produces valid output: FAILED"
    exit 1
fi

if $TIQ check "$TMP/loop_out.tiq" 2>/dev/null; then
    echo "formatter produces valid loop code: passed"
else
    echo "formatter produces valid loop code: FAILED"
    exit 1
fi

if $TIQ check "$TMP/fn_out.tiq" 2>/dev/null; then
    echo "formatter produces valid function code: passed"
else
    echo "formatter produces valid function code: FAILED"
    exit 1
fi

if $TIQ check "$TMP/stream_out.tiq" 2>/dev/null; then
    echo "formatter produces valid stream code: passed"
else
    echo "formatter produces valid stream code: FAILED"
    exit 1
fi

rm -rf "$TMP"
echo "formatter tests: ok"
