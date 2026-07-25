#!/bin/sh
# Test the Tiq run command

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-run-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "=== Testing run ==="

# Test run hello
OUTPUT=$($TIQ run examples/hello.tiq)
EXPECTED="Hello from Tiq"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run hello: passed"
else
    echo "run hello failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run arithmetic
cat > "$TMP/arith.tiq" << 'EOF'
!(1 + 2)
EOF
OUTPUT=$($TIQ run "$TMP/arith.tiq")
EXPECTED="3"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run arithmetic: passed"
else
    echo "run arithmetic failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run with variables
cat > "$TMP/vars.tiq" << 'EOF'
x <- 42
!x
EOF
OUTPUT=$($TIQ run "$TMP/vars.tiq")
EXPECTED="42"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run variables: passed"
else
    echo "run variables failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run with loops
cat > "$TMP/loop.tiq" << 'EOF'
x <- 0
[0..5 | x += i]
!x
EOF
OUTPUT=$($TIQ run "$TMP/loop.tiq")
EXPECTED="10"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run loop: passed"
else
    echo "run loop failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run with function
cat > "$TMP/fn.tiq" << 'EOF'
add a b -> a + b
!add(3, 4)
EOF
OUTPUT=$($TIQ run "$TMP/fn.tiq")
EXPECTED="7"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run function: passed"
else
    echo "run function failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run with stream generator
cat > "$TMP/stream.tiq" << 'EOF'
fib = [0, 1, ... a + b]
!fib[10]
EOF
OUTPUT=$($TIQ run "$TMP/stream.tiq")
EXPECTED="55"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run stream: passed"
else
    echo "run stream failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run with array
cat > "$TMP/array.tiq" << 'EOF'
xs <- [1, 2, 3]
!xs[0]
EOF
OUTPUT=$($TIQ run "$TMP/array.tiq")
EXPECTED="1"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run array: passed"
else
    echo "run array failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run with conditional
cat > "$TMP/cond.tiq" << 'EOF'
x <- 5
y <- x > 3 ? 10 : 20
!y
EOF
OUTPUT=$($TIQ run "$TMP/cond.tiq")
EXPECTED="10"
if [ "$OUTPUT" = "$EXPECTED" ]; then
    echo "run conditional: passed"
else
    echo "run conditional failed"
    echo "expected: $EXPECTED"
    echo "got: $OUTPUT"
    exit 1
fi

# Test run on non-existent file
if $TIQ run "$TMP/nonexistent.tiq" 2>/dev/null; then
    echo "run should have failed for non-existent file"
    exit 1
fi
echo "run non-existent: passed (as expected)"

# Test run on invalid code
printf '! ' > "$TMP/invalid.tiq"
if $TIQ run "$TMP/invalid.tiq" 2>/dev/null; then
    echo "run should have failed for invalid code"
    exit 1
fi
echo "run invalid code: passed (as expected)"

echo "run tests: ok"
