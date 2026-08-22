#!/bin/sh
# M19.2: Structured Concurrency & Channel tests (std/sync.tiq)
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-concurrency-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "concurrency: FAIL $1" >&2
  shift
  for f in "$@"; do
    echo "--- $f" >&2
    cat "$f" >&2 || true
  done
  exit 1
}

# Test 1: Check syntax
if ! "$TIQ" check std/sync.tiq > /dev/null 2>&1; then
  fail "check std/sync.tiq failed"
fi

if ! "$TIQ" check examples/concurrency_demo.tiq > /dev/null 2>&1; then
  fail "check examples/concurrency_demo.tiq failed"
fi

# Test 2: Run examples/concurrency_demo.tiq end-to-end
EXPECTED="4
3
10
20
30
0
1"

OUT_FILE="$TMP_DIR/demo.out"
ERR_FILE="$TMP_DIR/demo.err"
EXP_FILE="$TMP_DIR/demo.expected"

if ! "$TIQ" run examples/concurrency_demo.tiq > "$OUT_FILE" 2> "$ERR_FILE"; then
  fail "run examples/concurrency_demo.tiq failed" "$ERR_FILE"
fi

printf '%s\n' "$EXPECTED" > "$EXP_FILE"
if ! cmp -s "$EXP_FILE" "$OUT_FILE"; then
  fail "concurrency demo output mismatch" "$EXP_FILE" "$OUT_FILE"
fi

# Test 3: Channel edge cases (closed channel, capacity 1, bounds)
cat > "$TMP_DIR/edge_cases.tiq" << 'EOF'
import "std/sync.tiq"

ch = chan_new(1)
print(chan_cap(ch))
print(chan_len(ch))

// Send into cap-1 channel
s1 = chan_send(ch, 999)
print(s1)
print(chan_len(ch))

// Recv from cap-1 channel
r1 = chan_recv(ch)
print(r1)
print(chan_len(ch))

// Close channel
chan_close(ch)
// Sending to closed channel returns -1
s2 = chan_send(ch, 123)
print(s2)

// Receiving from empty closed channel returns 0
r2 = chan_recv(ch)
print(r2)

chan_free(ch)
print(1)
EOF

EXPECTED_EDGE="1
0
0
1
999
0
-1
0
1"

if ! "$TIQ" run "$TMP_DIR/edge_cases.tiq" > "$TMP_DIR/edge.out" 2> "$TMP_DIR/edge.err"; then
  fail "run edge_cases.tiq failed" "$TMP_DIR/edge.err"
fi

printf '%s\n' "$EXPECTED_EDGE" > "$TMP_DIR/edge.expected"
if ! cmp -s "$TMP_DIR/edge.expected" "$TMP_DIR/edge.out"; then
  fail "edge_cases output mismatch" "$TMP_DIR/edge.expected" "$TMP_DIR/edge.out"
fi

echo "concurrency: ok"
