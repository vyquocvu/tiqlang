#!/bin/sh
# Test the Tiq check command

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-check-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "=== Testing check ==="

# Test check on valid files
$TIQ check examples/hello.tiq
echo "check valid file: passed"

$TIQ check examples/fib.tiq
echo "check fib: passed"

# Test check on multiple files
$TIQ check examples/hello.tiq examples/fib.tiq
echo "check multiple files: passed"

# Test check on invalid code (should fail)
printf '! ' > "$TMP/invalid.tiq"
if $TIQ check "$TMP/invalid.tiq" 2>/dev/null; then
    echo "check should have failed for invalid code"
    exit 1
fi
echo "check invalid code rejected: passed"

# Test check on undefined symbol
printf '! undefined_sym' > "$TMP/undef.tiq"
if $TIQ check "$TMP/undef.tiq" 2>/dev/null; then
    echo "check should have failed for undefined symbol"
    exit 1
fi
echo "check undefined symbol rejected: passed"

# Test check on type mismatch
cat > "$TMP/type_err.tiq" << 'EOF'
x <- 1
y <- "string"
z <- x + y
EOF
if $TIQ check "$TMP/type_err.tiq" 2>/dev/null; then
    echo "check should have failed for type mismatch"
    exit 1
fi
echo "check type mismatch rejected: passed"

# Test check on move immutable
cat > "$TMP/move_imm.tiq" << 'EOF'
x = 1
y <- move x
EOF
if $TIQ check "$TMP/move_imm.tiq" 2>/dev/null; then
    echo "check should have failed for move immutable"
    exit 1
fi
echo "check move immutable rejected: passed"

# Test check on use after move
cat > "$TMP/use_after_move.tiq" << 'EOF'
x <- 1
y <- move x
print(x)
EOF
if $TIQ check "$TMP/use_after_move.tiq" 2>/dev/null; then
    echo "check should have failed for use after move"
    exit 1
fi
echo "check use after move rejected: passed"

# Test check on break outside loop
cat > "$TMP/break_outside.tiq" << 'EOF'
break
EOF
if $TIQ check "$TMP/break_outside.tiq" 2>/dev/null; then
    echo "check should have failed for break outside loop"
    exit 1
fi
echo "check break outside loop rejected: passed"

# Test check on defer outside block
cat > "$TMP/defer_outside.tiq" << 'EOF'
defer print(1)
EOF
if $TIQ check "$TMP/defer_outside.tiq" 2>/dev/null; then
    echo "check should have failed for defer outside block"
    exit 1
fi
echo "check defer outside block rejected: passed"

# Test check on non-existent file
if $TIQ check "$TMP/nonexistent.tiq" 2>/dev/null; then
    echo "check should have failed for non-existent file"
    exit 1
fi
echo "check non-existent file rejected: passed"

echo "check tests: ok"
