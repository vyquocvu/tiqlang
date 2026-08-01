#!/bin/sh
# M13.5-P0: executable differential harness for the first self-hosted C11
# emitter slice. M13.6 owns byte identity with the reference C emitter; this
# package pins deterministic self-host output and observable program behavior.
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
SELFHOST="build/tiq-emit-c-selfhost"
TMP_DIR="${TMPDIR:-/tmp}/tiq-selfhost-emit-c-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/emit_c_main.tiq -o "$SELFHOST" 2>"$TMP_DIR/build.err"; then
  echo "selfhost_emit_c: FAIL (cannot build src/tiq/emit_c_main.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0
count=0

case_run() {
  name="$1"
  source="$2"
  count=$((count + 1))
  printf '%s\n' "$source" >"$TMP_DIR/$name.tiq"

  "$TIQ" run "$TMP_DIR/$name.tiq" >"$TMP_DIR/$name.ref.out" 2>"$TMP_DIR/$name.ref.err"
  ref_rc=$?
  "$SELFHOST" "$TMP_DIR/$name.tiq" >"$TMP_DIR/$name.1.c" 2>"$TMP_DIR/$name.emit.err"
  emit_rc=$?
  "$SELFHOST" "$TMP_DIR/$name.tiq" >"$TMP_DIR/$name.2.c" 2>>"$TMP_DIR/$name.emit.err"
  if [ "$emit_rc" -ne 0 ] || ! cmp -s "$TMP_DIR/$name.1.c" "$TMP_DIR/$name.2.c"; then
    echo "selfhost_emit_c: FAIL $name (emission failed or was nondeterministic)" >&2
    fail=1
    return
  fi
  if ! "$CC_BIN" -std=c11 -Wall -Wextra -Wpedantic -Werror "$TMP_DIR/$name.1.c" -o "$TMP_DIR/$name.bin" 2>"$TMP_DIR/$name.cc.err"; then
    echo "selfhost_emit_c: FAIL $name (generated C did not compile)" >&2
    cat "$TMP_DIR/$name.cc.err" >&2
    fail=1
    return
  fi
  "$TMP_DIR/$name.bin" >"$TMP_DIR/$name.got.out" 2>"$TMP_DIR/$name.got.err"
  got_rc=$?
  if [ "$ref_rc" -ne "$got_rc" ] || ! cmp -s "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" || ! cmp -s "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err"; then
    echo "selfhost_emit_c: FAIL $name (observable behavior mismatch)" >&2
    diff "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" >&2 || true
    diff "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err" >&2 || true
    fail=1
  fi
}

case_run "string" 'msg = "hello"
print(msg)'
case_run "arithmetic" 'x = (7 + 5) * 3 - 4 / 2
print(x)'
case_run "conditional" 'x = 9
print(x > 4 ? 11 : 22)'
case_run "bool" 'print(3 < 5 && true)'
case_run "range_loop" 'sum <- 0
[j <- 0..6] { sum += j }
print(sum)'
case_run "while_loop" 'n <- 0
[n < 4] { n += 1 }
print(n)'
case_run "function" 'add a b -> a + b
print(add(20, 22))'
case_run "array_read_len" 'xs = [4, 8, 15, 16]
print(xs[2])
print(len(xs))'
case_run "array_fill" 'xs = [7; 3]
print(xs[0] + xs[1] + xs[2])'
case_run "array_assign" 'xs <- [1, 2, 3]
xs[1] <- 9
xs[2] += 4
print(xs[0] + xs[1] + xs[2])'
case_run "array_oob_read" 'xs = [1, 2]
print(xs[2])'
case_run "array_oob_write" 'xs <- [1, 2]
xs[3] <- 9'

# Unsupported backend paths must fail closed before writing partial C.
printf '%s\n' 'xs = [1, 2, 3]
tail = xs[1..]
print(len(tail))' >"$TMP_DIR/unsupported_slice.tiq"
"$SELFHOST" "$TMP_DIR/unsupported_slice.tiq" >"$TMP_DIR/unsupported_slice.out" 2>"$TMP_DIR/unsupported_slice.err"
unsupported_rc=$?
if [ "$unsupported_rc" -eq 0 ] || [ -s "$TMP_DIR/unsupported_slice.out" ] ||
   ! grep -q 'unsupported_slice.tiq:2: error\[E07\]: self-hosted C emitter does not support slice expressions yet' "$TMP_DIR/unsupported_slice.err"; then
  echo "selfhost_emit_c: FAIL unsupported_slice (did not fail closed with located E07)" >&2
  cat "$TMP_DIR/unsupported_slice.err" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "selfhost_emit_c: failed" >&2
  exit 1
fi
echo "selfhost_emit_c: ok ($count core cases + fail-closed preflight)"
