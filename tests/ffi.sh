#!/bin/sh
# M16.1/M16.2: FFI end-to-end — extern "C" declarations emit C prototypes
# at the pinned pass position, link against host libraries via -l/-L,
# run correctly, and fail closed when a symbol is missing
# (LANGUAGE_SPEC §7.1, CLI.md link options).
# M16.3: tiq emit-header generates a deterministic C header for a
# definitions-only library, and tiq emit-c --lib omits the entry point
# so the library links into a C host program (LANGUAGE_SPEC §18.3).
set -eu

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-ffi-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

assert_run() {
  name="$1"
  source="$2"
  expected="$3"
  shift 3
  input="$TMP_DIR/$name.tiq"
  printf '%s\n' "$source" > "$input"
  set +e
  OUTPUT=$("$TIQ" run "$input" "$@" 2>"$TMP_DIR/$name.err")
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    echo "ffi: $name run failed" >&2
    cat "$TMP_DIR/$name.err" >&2
    exit 1
  fi
  if [ "$OUTPUT" != "$expected" ]; then
    echo "ffi: $name output mismatch" >&2
    echo "expected: $expected" >&2
    echo "got: $OUTPUT" >&2
    exit 1
  fi
  echo "ffi $name: passed"
}

# libc symbols link without extra flags and map through the C ABI table.
assert_run "ffi_llabs" 'extern "C" llabs x:i64 -> i64
print(llabs(3 - 10))' "7"

assert_run "ffi_strlen" 'extern "C" strlen s:str -> i64
print(strlen("tiq lang"))' "8"

# -l/-L are repeatable link options forwarded to the host compiler.
assert_run "ffi_sqrt_libm" 'extern "C" sqrt x:f64 -> f64
print(sqrt(9.0))' "3" -l m -L "$TMP_DIR"

# `tiq build` accepts the same link options.
printf '%s\n' 'extern "C" sqrt x:f64 -> f64
print(sqrt(16.0))' > "$TMP_DIR/sqrt_build.tiq"
"$TIQ" build "$TMP_DIR/sqrt_build.tiq" -o "$TMP_DIR/sqrt_build" -l m
OUT=$("$TMP_DIR/sqrt_build")
if [ "$OUT" != "4" ]; then
  echo "ffi: sqrt_build output mismatch (got $OUT)" >&2
  exit 1
fi
echo "ffi sqrt_build: passed"

# Prototype emission: exact lines and pass position (after the enum
# constants, before user function forward declarations).
cat > "$TMP_DIR/ffi_emit.tiq" << 'EOF'
extern "C" llabs x:i64 -> i64
extern "C" tiq_ffi_zero_probe -> i64
extern "C" getpid -> i64
enum Color { Red }
helper x:i64 -> i64 -> llabs(x)
print(helper(0 - 5))
EOF
"$TIQ" emit-c "$TMP_DIR/ffi_emit.tiq" > "$TMP_DIR/ffi_emit.c"
if ! grep -qxF 'extern int64_t llabs(int64_t x);' "$TMP_DIR/ffi_emit.c"; then
  echo "ffi: missing llabs prototype" >&2
  exit 1
fi
# Zero-param externs take (void). A unique name pins the shape because
# libc names like getpid are shadowed by preamble headers: their
# fixed-width redeclaration would conflict, so the pass suppresses them
# and the system header declaration serves for codegen/linking.
if ! grep -qxF 'extern int64_t tiq_ffi_zero_probe(void);' "$TMP_DIR/ffi_emit.c"; then
  echo "ffi: missing zero-param prototype" >&2
  exit 1
fi
if grep -q 'extern .*getpid' "$TMP_DIR/ffi_emit.c"; then
  echo "ffi: header-shadowed getpid prototype must be suppressed" >&2
  exit 1
fi
enum_line=$(grep -n 'tiq_enum_Color_Red' "$TMP_DIR/ffi_emit.c" | head -1 | cut -d: -f1)
proto_line=$(grep -nxF 'extern int64_t llabs(int64_t x);' "$TMP_DIR/ffi_emit.c" | head -1 | cut -d: -f1)
fwd_line=$(grep -nxF 'int64_t helper(int64_t x);' "$TMP_DIR/ffi_emit.c" | head -1 | cut -d: -f1)
if [ -z "$enum_line" ] || [ -z "$proto_line" ] || [ -z "$fwd_line" ]; then
  echo "ffi: emit-c anchors not found" >&2
  exit 1
fi
if [ "$proto_line" -le "$enum_line" ] || [ "$proto_line" -ge "$fwd_line" ]; then
  echo "ffi: extern prototypes out of pass order (enum=$enum_line proto=$proto_line fwd=$fwd_line)" >&2
  exit 1
fi
OUT=$("$TIQ" run "$TMP_DIR/ffi_emit.tiq")
if [ "$OUT" != "5" ]; then
  echo "ffi: ffi_emit output mismatch (got $OUT)" >&2
  exit 1
fi
echo "ffi emit_c_prototypes: passed"

# Fail closed: an undefined extern symbol must fail at link time and
# leave no executable behind.
printf '%s\n' 'extern "C" tiq_ffi_missing_symbol_xyz x:i64 -> i64
print(tiq_ffi_missing_symbol_xyz(1))' > "$TMP_DIR/missing.tiq"
if "$TIQ" build "$TMP_DIR/missing.tiq" -o "$TMP_DIR/missing" 2>"$TMP_DIR/missing.err"; then
  echo "ffi: missing symbol should fail to link" >&2
  exit 1
fi
if [ -e "$TMP_DIR/missing" ]; then
  echo "ffi: missing symbol unexpectedly produced an executable" >&2
  exit 1
fi
echo "ffi missing_symbol: passed (as expected)"

# ASan/UBSan clean on the emitted C (host-compiled independently).
"$TIQ" emit-c "$TMP_DIR/ffi_llabs.tiq" > "$TMP_DIR/asan.c"
"$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/asan.c" -o "$TMP_DIR/asan"
OUT=$("$TMP_DIR/asan")
if [ "$OUT" != "7" ]; then
  echo "ffi: sanitized run output mismatch (got $OUT)" >&2
  exit 1
fi
echo "ffi asan_clean: passed"

# --- M16.3: emit-header / emit-c --lib --------------------------------

# Library fixture: FFI-safe functions export; vec/borrow signatures and
# extern decls stay internal but the file still compiles as a library.
cat > "$TMP_DIR/hlib.tiq" << 'EOF'
struct Point { x: i64, y: i64 }
add a:i64 b:i64 -> i64 -> a + b
scale p:Point k:i64 -> Point -> Point { x: p.x * k, y: p.y * k }
greet n:i64 -> str -> "hi"
flag n:i64 -> bool -> n > 0
ratio a:f64 b:f64 -> f64 -> a / b
internal v:vec[i64] -> i64 -> 0
helper x:&i64 -> i64 -> x
extern "C" llabs x:i64 -> i64
abs_wrap x:i64 -> i64 -> llabs(x)
EOF

# Header golden: byte-exact shape, declaration order, ABI spelling.
cat > "$TMP_DIR/hlib.h.golden" << 'EOF'
/* Generated by tiq emit-header from hlib. Do not edit. */
/* Returned str values are Tiq-owned: callers must not free them (leak, never dangle). */
#ifndef TIQ_HLIB_H
#define TIQ_HLIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t x;
    int64_t y;
} Point;

int64_t add(int64_t a, int64_t b);
Point scale(Point p, int64_t k);
const char * greet(int64_t n);
int64_t flag(int64_t n);
double ratio(double a, double b);
int64_t abs_wrap(int64_t x);

#ifdef __cplusplus
}
#endif

#endif
EOF
"$TIQ" emit-header "$TMP_DIR/hlib.tiq" > "$TMP_DIR/hlib.h"
if ! cmp -s "$TMP_DIR/hlib.h" "$TMP_DIR/hlib.h.golden"; then
  echo "ffi: emit-header golden mismatch" >&2
  diff "$TMP_DIR/hlib.h" "$TMP_DIR/hlib.h.golden" >&2 || true
  exit 1
fi
# -o writes the same bytes to a file.
"$TIQ" emit-header "$TMP_DIR/hlib.tiq" -o "$TMP_DIR/hlib_o.h"
if ! cmp -s "$TMP_DIR/hlib_o.h" "$TMP_DIR/hlib.h.golden"; then
  echo "ffi: emit-header -o mismatch" >&2
  exit 1
fi
echo "ffi emit_header_golden: passed"

# Fail closed (E31): a top-level statement disqualifies the library in
# both modes; stdout stays empty and the diagnostic carries the location.
printf '%s\n' 'add a:i64 b:i64 -> i64 -> a + b' 'print(add(1, 2))' > "$TMP_DIR/notlib.tiq"
set +e
OUT=$("$TIQ" emit-header "$TMP_DIR/notlib.tiq" 2>"$TMP_DIR/notlib.err")
rc=$?
set -e
if [ "$rc" -eq 0 ] || [ -n "$OUT" ]; then
  echo "ffi: emit-header must fail closed on top-level statements" >&2
  exit 1
fi
if ! grep -qF "$TMP_DIR/notlib.tiq:2: error[E31]: library requires definitions only; top-level statement is not allowed" "$TMP_DIR/notlib.err"; then
  echo "ffi: missing E31 diagnostic" >&2
  cat "$TMP_DIR/notlib.err" >&2
  exit 1
fi
set +e
"$TIQ" emit-c --lib "$TMP_DIR/notlib.tiq" > "$TMP_DIR/notlib.c" 2>"$TMP_DIR/notlib2.err"
rc=$?
set -e
if [ "$rc" -eq 0 ] || [ -s "$TMP_DIR/notlib.c" ]; then
  echo "ffi: emit-c --lib must fail closed on top-level statements" >&2
  exit 1
fi
echo "ffi library_fail_closed: passed"

# Usage fail-closed: missing file or unknown flags exit 2 with usage.
for args in "emit-header" "emit-header --bogus x.tiq" "emit-c --lib" "emit-c --lib a.tiq b.tiq"; do
  set +e
  "$TIQ" $args >"$TMP_DIR/usage.out" 2>"$TMP_DIR/usage.err"
  rc=$?
  set -e
  if [ "$rc" -ne 2 ] || ! grep -q 'usage:' "$TMP_DIR/usage.err"; then
    echo "ffi: usage fail-closed violated for: tiq $args (rc=$rc)" >&2
    exit 1
  fi
done
echo "ffi usage_fail_closed: passed"

# emit-c --lib pins: no entry point, definitions and extern prototypes
# intact, and the output host-compiles as a translation unit. Generated
# code compiles with the same plain flags tiq itself uses (the runtime
# preamble carries statics a small library never touches).
"$TIQ" emit-c --lib "$TMP_DIR/hlib.tiq" > "$TMP_DIR/hlib.c"
if grep -q 'int main(' "$TMP_DIR/hlib.c"; then
  echo "ffi: --lib output must omit int main" >&2
  exit 1
fi
if ! grep -qxF 'int64_t add(int64_t a, int64_t b);' "$TMP_DIR/hlib.c"; then
  echo "ffi: --lib output missing function forward declaration" >&2
  exit 1
fi
if ! grep -qxF 'extern int64_t llabs(int64_t x);' "$TMP_DIR/hlib.c"; then
  echo "ffi: --lib output missing extern prototype" >&2
  exit 1
fi
"$CC_BIN" -std=c11 -c "$TMP_DIR/hlib.c" -o "$TMP_DIR/hlib_check.o"
echo "ffi emit_c_lib_pins: passed"

# End-to-end embedding: a C host program includes the generated header,
# links the --lib translation unit, and calls through the C ABI.
cat > "$TMP_DIR/host.c" << 'EOF'
#include <stdio.h>
#include "hlib.h"

int main(void) {
    Point p = {2, 3};
    Point q = scale(p, 4);
    printf("%lld\n", (long long)add(20, 22));
    printf("%lld %lld\n", (long long)q.x, (long long)q.y);
    printf("%s\n", greet(1));
    printf("%lld\n", (long long)flag(5));
    printf("%lld\n", (long long)abs_wrap(0 - 9));
    return 0;
}
EOF
"$CC_BIN" -std=c11 -Wall -Wextra -Wpedantic -Werror -I "$TMP_DIR" \
  -c "$TMP_DIR/host.c" -o "$TMP_DIR/host.o"
"$CC_BIN" -std=c11 "$TMP_DIR/host.o" "$TMP_DIR/hlib.c" -o "$TMP_DIR/host"
OUT=$("$TMP_DIR/host")
EXPECTED="42
8 12
hi
1
9"
if [ "$OUT" != "$EXPECTED" ]; then
  echo "ffi: embedding output mismatch" >&2
  echo "expected: $EXPECTED" >&2
  echo "got: $OUT" >&2
  exit 1
fi
echo "ffi embedding_end_to_end: passed"

echo "ffi: ok"
