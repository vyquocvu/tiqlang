#!/bin/sh
# M13.2-S1: differential harness for the self-hosted Tiq lexer. Builds
# src/tiq/lexer_main.tiq with the C bootstrap compiler, then runs both
# `tiq dump-tokens F` and the self-hosted lexer over every fixture in
# examples/, examples/leetcode/, and tests/tiq/. stdout, stderr, and the
# exit code must all match byte-for-byte — including fixtures the lexer
# rejects (E01/E02/E03 diagnostics must agree on both sides).
set -u

TIQ="${TIQ:-./build/tiq}"
SELFHOST="build/tiq-lexer-selfhost"
TMP_DIR="${TMPDIR:-/tmp}/tiq-selfhost-lexer-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/lexer_main.tiq -o "$SELFHOST" 2> "$TMP_DIR/build.err"; then
  echo "selfhost_lexer: FAIL (cannot build src/tiq/lexer_main.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

FIXTURES=$(ls examples/*.tiq tests/tiq/*.tiq 2>/dev/null)
if [ -d examples/leetcode ]; then
  FIXTURES="$FIXTURES
$(ls examples/leetcode/*.tiq 2>/dev/null)"
fi

fail=0
count=0
for src in $FIXTURES; do
  count=$((count + 1))
  name=$(basename "$src" .tiq)

  "$TIQ" dump-tokens "$src" > "$TMP_DIR/$name.ref.out" 2> "$TMP_DIR/$name.ref.err"
  ref_rc=$?
  "./$SELFHOST" "$src" > "$TMP_DIR/$name.got.out" 2> "$TMP_DIR/$name.got.err"
  got_rc=$?

  if [ "$ref_rc" -ne "$got_rc" ]; then
    echo "selfhost_lexer: FAIL $src (exit code: reference $ref_rc, selfhost $got_rc)" >&2
    fail=1
  fi
  if ! cmp -s "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out"; then
    echo "selfhost_lexer: FAIL $src (stdout mismatch)" >&2
    diff "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" >&2 || true
    fail=1
  fi
  if ! cmp -s "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err"; then
    echo "selfhost_lexer: FAIL $src (stderr mismatch)" >&2
    diff "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err" >&2 || true
    fail=1
  fi
done

if [ "$count" -eq 0 ]; then
  echo "selfhost_lexer: FAIL (no fixtures found)" >&2
  exit 1
fi
if [ "$fail" -ne 0 ]; then
  echo "selfhost_lexer: failed" >&2
  exit 1
fi
echo "selfhost_lexer: ok ($count fixtures)"
