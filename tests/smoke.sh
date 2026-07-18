#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-smoke-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

./build/tiq --version
./build/tiq emit-c examples/hello.tiq > "$TMP_DIR/hello.c"
./build/tiq build examples/hello.tiq -o "$TMP_DIR/hello"

OUTPUT="$($TMP_DIR/hello)"
[ "$OUTPUT" = "Hello from Tiq" ]

printf '%s\n' 'x = 1' > "$TMP_DIR/invalid.tiq"
if ./build/tiq build "$TMP_DIR/invalid.tiq" -o "$TMP_DIR/invalid" 2>/dev/null; then
  echo "expected invalid program to fail" >&2
  exit 1
fi

echo "smoke: ok"
