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

SPECIAL_OUTPUT="$TMP_DIR/hello path;quote\"name"
./build/tiq build examples/hello.tiq -o "$SPECIAL_OUTPUT"
SPECIAL_RESULT="$("$SPECIAL_OUTPUT")"
[ "$SPECIAL_RESULT" = "Hello from Tiq" ]

SPECIAL_TMPDIR="$TMP_DIR/tmp path;quote\"dir"
mkdir -p "$SPECIAL_TMPDIR"
TMPDIR="$SPECIAL_TMPDIR" ./build/tiq build examples/hello.tiq -o "$TMP_DIR/tmpdir-hello"
TMPDIR_RESULT="$($TMP_DIR/tmpdir-hello)"
[ "$TMPDIR_RESULT" = "Hello from Tiq" ]
if find "$SPECIAL_TMPDIR" -mindepth 1 -print | grep . >/dev/null; then
  echo "temporary C file was not removed" >&2
  exit 1
fi

if env CC="$TMP_DIR/missing-cc" ./build/tiq build examples/hello.tiq -o "$TMP_DIR/no-compiler" 2>"$TMP_DIR/missing-cc.err"; then
  echo "expected missing host C compiler to fail" >&2
  exit 1
fi
if [ -e "$TMP_DIR/no-compiler" ]; then
  echo "missing host C compiler unexpectedly produced an executable" >&2
  exit 1
fi
grep 'cannot execute host C compiler' "$TMP_DIR/missing-cc.err" >/dev/null

printf '%s\n' 'x = 1' > "$TMP_DIR/invalid.tiq"
if ./build/tiq build "$TMP_DIR/invalid.tiq" -o "$TMP_DIR/invalid" 2>/dev/null; then
  echo "expected invalid program to fail" >&2
  exit 1
fi

echo "smoke: ok"
