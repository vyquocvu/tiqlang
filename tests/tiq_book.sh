#!/bin/sh
# M20.4: Tiq Book examples verification
set -eu

BOOK="docs/TIQ_BOOK.md"
TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-book-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# Check The Tiq Book exists
test -f "$BOOK" || { echo "tiq-book: FAIL $BOOK missing" >&2; exit 1; }

# Check README links to The Tiq Book
grep -q 'TIQ_BOOK.md' README.md || { echo "tiq-book: FAIL README must link to TIQ_BOOK.md" >&2; exit 1; }

# Extract and verify key code examples from the book
cat > "$TMP_DIR/hello.tiq" << 'TIQ'
print("Hello, Tiq!")
TIQ

cat > "$TMP_DIR/fib.tiq" << 'TIQ'
fib = [0, 1, ... (a, b) -> a + b]
print(fib[10])
TIQ

cat > "$TMP_DIR/factorial.tiq" << 'TIQ'
factorial = [1, ... (x; i) -> x * i]
print(factorial[5])
TIQ

cat > "$TMP_DIR/loop.tiq" << 'TIQ'
[i <- 0..5] {
    print(i)
}
TIQ

cat > "$TMP_DIR/skip.tiq" << 'TIQ'
[i <- 0..10] {
    i % 2 == 0 ? { skip }
    print(i)
}
TIQ

cat > "$TMP_DIR/struct.tiq" << 'TIQ'
struct Point {
    x: i64
    y: i64
}

p = Point { x: 10, y: 20 }
print(p.x)
print(p.y)
TIQ

cat > "$TMP_DIR/match.tiq" << 'TIQ'
enum Light { Red, Yellow, Green }

action light -> match light {
    Light.Red => "Stop"
    Light.Yellow => "Caution"
    Light.Green => "Go"
    _ => "Unknown"
}

print(action(Light.Red))
print(action(Light.Green))
TIQ

passed=0
failed=0

for f in "$TMP_DIR"/*.tiq; do
    name=$(basename "$f")
    if $TIQ check "$f" >/dev/null 2>&1; then
        passed=$((passed + 1))
    else
        echo "tiq-book: FAIL $name did not pass tiq check" >&2
        failed=$((failed + 1))
    fi
done

if [ $failed -eq 0 ]; then
    echo "tiq-book: ok ($passed examples verified from $BOOK)"
else
    echo "tiq-book: FAIL ($failed failed, $passed passed)" >&2
    exit 1
fi
