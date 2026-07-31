#!/bin/sh
# M13 Phase 0: code generation must be deterministic. For a fixed fixture
# list, `tiq emit-c` is run twice and the outputs must be byte-identical;
# any difference names the offending file and fails the suite.
set -u

TMP_DIR="${TMPDIR:-/tmp}/tiq-determinism-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

FIXTURES="examples/hello.tiq
examples/fib.tiq
examples/structs.tiq
examples/primes.tiq
tests/tiq/loop_test.tiq"

fail=0
for src in $FIXTURES; do
  name=$(basename "$src" .tiq)
  if ! ./build/tiq emit-c "$src" > "$TMP_DIR/$name.1.c" 2>"$TMP_DIR/$name.err"; then
    echo "determinism: FAIL $src (emit-c error)" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
    continue
  fi
  if ! ./build/tiq emit-c "$src" > "$TMP_DIR/$name.2.c" 2>"$TMP_DIR/$name.err"; then
    echo "determinism: FAIL $src (emit-c error on second run)" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
    continue
  fi
  if ! cmp -s "$TMP_DIR/$name.1.c" "$TMP_DIR/$name.2.c"; then
    echo "determinism: FAIL $src (emit-c output differs between runs)" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "determinism: failed" >&2
  exit 1
fi
echo "determinism: ok"
