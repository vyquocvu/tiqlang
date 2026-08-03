#!/bin/sh
# M16.1/M16.2: FFI end-to-end — extern "C" declarations emit C prototypes
# at the pinned pass position, link against host libraries via -l/-L,
# run correctly, and fail closed when a symbol is missing
# (LANGUAGE_SPEC §7.1, CLI.md link options).
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

echo "ffi: ok"
