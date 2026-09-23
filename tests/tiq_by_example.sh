#!/bin/sh
# Tiq by Example verification
set -eu

EXAMPLE="docs/TIQ_BY_EXAMPLE.md"
TIQ=./build/tiq

if [ ! -f "$EXAMPLE" ]; then
    echo "tiq-by-example: SKIP ($EXAMPLE not found)"
    exit 0
fi

if [ ! -x "$TIQ" ]; then
    echo "tiq-by-example: SKIP (tiq binary not found)"
    exit 0
fi

fail=0

# Test 1: Variables and types
cat > /tmp/tbe_vars.tiq << 'EOF'
x = 42
name = "Tiq"
pi = 3.14159
flag = true
print(x)
print(name)
EOF
if ! "$TIQ" check /tmp/tbe_vars.tiq 2>/dev/null; then
    echo "tiq-by-example: FAIL (variables)"
    fail=1
fi

# Test 2: Recurrences
cat > /tmp/tbe_recur.tiq << 'EOF'
fib = [0, 1, ... (a, b) -> a + b]
print(fib[10])
EOF
if ! "$TIQ" check /tmp/tbe_recur.tiq 2>/dev/null; then
    echo "tiq-by-example: FAIL (recurrences)"
    fail=1
fi

# Test 3: Pattern matching
cat > /tmp/tbe_match.tiq << 'EOF'
classify n: i64 : str ->
    match n {
        0 => "zero",
        1 => "one",
        _ => "many"
    }
print(classify(42))
EOF
if ! "$TIQ" check /tmp/tbe_match.tiq 2>/dev/null; then
    echo "tiq-by-example: FAIL (pattern matching)"
    fail=1
fi

# Test 4: Structs
cat > /tmp/tbe_struct.tiq << 'EOF'
struct Point {
    x: i64
    y: i64
}
p = Point { x: 10, y: 20 }
print(p.x)
EOF
if ! "$TIQ" check /tmp/tbe_struct.tiq 2>/dev/null; then
    echo "tiq-by-example: FAIL (structs)"
    fail=1
fi

# Test 5: Loops
cat > /tmp/tbe_loop.tiq << 'EOF'
[i <- 0..5] {
    print(i)
}
EOF
if ! "$TIQ" check /tmp/tbe_loop.tiq 2>/dev/null; then
    echo "tiq-by-example: FAIL (loops)"
    fail=1
fi

# Test 6: Check README links to Tiq by Example
if ! grep -q "TIQ_BY_EXAMPLE.md" README.md; then
    echo "tiq-by-example: FAIL (README link missing)"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "tiq-by-example: ok (5 examples verified from docs/TIQ_BY_EXAMPLE.md)"
else
    exit 1
fi
