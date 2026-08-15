#!/bin/sh
# M19.6: SQLite3 connector tests (std/sqlite.tiq)
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-sqlite-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "sqlite: FAIL $1" >&2
  shift
  for f in "$@"; do
    echo "--- $f" >&2
    cat "$f" >&2 || true
  done
  exit 1
}

# Test 1: Check example syntax
if ! "$TIQ" check examples/sqlite_demo.tiq > /dev/null 2>&1; then
  fail "check examples/sqlite_demo.tiq failed"
fi

# Test 2: Run examples/sqlite_demo.tiq end-to-end with -l sqlite3
EXPECTED="1
1
1
1
1
Alice
95
3
Charlie
91
2
Bob
82"

OUT_FILE="$TMP_DIR/demo.out"
ERR_FILE="$TMP_DIR/demo.err"
EXP_FILE="$TMP_DIR/demo.expected"

if ! "$TIQ" run examples/sqlite_demo.tiq -l sqlite3 > "$OUT_FILE" 2> "$ERR_FILE"; then
  fail "run examples/sqlite_demo.tiq failed" "$ERR_FILE"
fi

printf '%s\n' "$EXPECTED" > "$EXP_FILE"
if ! cmp -s "$EXP_FILE" "$OUT_FILE"; then
  fail "sqlite demo output mismatch" "$EXP_FILE" "$OUT_FILE"
fi

echo "sqlite: ok"
