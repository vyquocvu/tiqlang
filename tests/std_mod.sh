#!/bin/sh
# M15: Standard library modularization — verifies gated domain builtins
# (json_*, net_*, http_*, ev_*) require `import "std/<mod>.tiq"`, core
# builtins remain always available, cwd fallback resolves std/ from any
# depth, and wrapper functions produce correct results.
set -eu

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-std-mod-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "std_mod: FAIL $1" >&2
  shift
  for f in "$@"; do
    echo "--- $f" >&2
    cat "$f" >&2 || true
  done
  exit 1
}

assert_runs() {
  name="$1"
  root="$2"
  expected="$3"
  out_file="$TMP_DIR/$name.out"
  err_file="$TMP_DIR/$name.err"
  exp_file="$TMP_DIR/$name.expected"
  if ! "$TIQ" run "$root" > "$out_file" 2>"$err_file"; then
    fail "$name (run failed)" "$err_file"
  fi
  printf '%s\n' "$expected" > "$exp_file"
  if ! cmp -s "$exp_file" "$out_file"; then
    fail "$name (output mismatch)" "$exp_file" "$out_file"
  fi
}

assert_rejects() {
  name="$1"
  root="$2"
  pattern="$3"
  err_file="$TMP_DIR/$name.err"
  exe_file="$TMP_DIR/$name.bin"
  if "$TIQ" build "$root" -o "$exe_file" 2>"$err_file"; then
    fail "$name (expected build to fail)" "$err_file"
  fi
  if ! grep -q "$pattern" "$err_file"; then
    echo "std_mod: FAIL $name (diagnostic mismatch)" >&2
    echo "expected pattern: $pattern" >&2
    echo "actual:" >&2
    cat "$err_file" >&2
    exit 1
  fi
}

# --- 1. Gated builtins rejected without import ---

cat > "$TMP_DIR/no_import.tiq" <<'EOF'
x <- json_parse_int("42")
print(x)
EOF
assert_rejects "gate_json" "$TMP_DIR/no_import.tiq" 'import "std/json.tiq"'

cat > "$TMP_DIR/no_import_net.tiq" <<'EOF'
fd <- net_listen(8080)
print(fd)
EOF
assert_rejects "gate_net" "$TMP_DIR/no_import_net.tiq" 'import "std/net.tiq"'

# ev_loop is ungated (zero-param cannot be wrapped); ev_add is gated.
cat > "$TMP_DIR/no_import_ev.tiq" <<'EOF'
r <- ev_add(0, 0)
print(r)
EOF
assert_rejects "gate_ev" "$TMP_DIR/no_import_ev.tiq" 'import "std/ev.tiq"'

cat > "$TMP_DIR/no_import_dl.tiq" <<'EOF'
h <- dl_open("x")
print(h)
EOF
assert_rejects "gate_dl" "$TMP_DIR/no_import_dl.tiq" 'import "std/dl.tiq"'

# --- 2. Gated builtins work with import ---

cat > "$TMP_DIR/with_import.tiq" <<'EOF'
import "std/json.tiq"
x <- json_parse_int("42")
print(x)
EOF
assert_runs "json_import" "$TMP_DIR/with_import.tiq" "42"

# --- 3. Core builtins work without any import ---

cat > "$TMP_DIR/core.tiq" <<'EOF'
s <- str_cat("hello", " world")
print(s)
print(len(s))
n <- 3 + 4
print(n)
EOF
assert_runs "core_builtins" "$TMP_DIR/core.tiq" "hello world
11
7"

# --- 4. std/ import resolves from nested directory (cwd fallback) ---

mkdir -p "$TMP_DIR/sub/deep"
cat > "$TMP_DIR/sub/deep/nested.tiq" <<'EOF'
import "std/json.tiq"
v <- json_parse_int("99")
print(v)
EOF
# Run from project root so std/ is findable via cwd fallback.
out_file="$TMP_DIR/nested.out"
err_file="$TMP_DIR/nested.err"
if ! "$TIQ" run "$TMP_DIR/sub/deep/nested.tiq" > "$out_file" 2>"$err_file"; then
  fail "cwd_fallback (run failed)" "$err_file"
fi
if ! grep -q "99" "$out_file"; then
  fail "cwd_fallback (output mismatch)" "$out_file"
fi

# --- 5. Wrapper functions produce correct results ---

cat > "$TMP_DIR/wrapper.tiq" <<'EOF'
import "std/json.tiq"
obj <- "{\"name\":\"tiq\",\"ver\":3}"
name <- json_get(obj, "name")
print(name)
has_ver <- json_has(obj, "ver")
print(has_ver)
has_no <- json_has(obj, "missing")
print(has_no)
arr <- "[10,20,30]"
print(json_arr_len(arr))
elem <- json_arr_get(arr, 1)
print(elem)
EOF
assert_runs "wrapper_results" "$TMP_DIR/wrapper.tiq" "tiq
true
false
3
20"

# Epic 1: allocator stdlib uses the existing C FFI boundary. Creation and
# allocation are fallible Results; reset/dealloc/destroy are deterministic
# status returns. General, arena, and pool strategies all share one handle API.
cat > "$TMP_DIR/allocator.tiq" <<'EOF'
import "std/alloc.tiq"

general = tiq_allocator_general()
gp = allocator_alloc(i64(general), 32, 8) ?? i64(0)
print(gp != i64(0))
print(allocator_dealloc(i64(general), gp, 32, 8))

arena_handle = arena(128) ?? i64(0)
ap = allocator_alloc(arena_handle, 16, 8) ?? i64(0)
print(ap != i64(0))
print(allocator_reset(arena_handle))
ap2 = allocator_alloc(arena_handle, 64, 8) ?? i64(0)
print(ap2 != i64(0))
print(allocator_destroy(arena_handle))

pool_handle = pool(16, 2) ?? i64(0)
p1 = allocator_alloc(pool_handle, 16, 8) ?? i64(0)
p2 = allocator_alloc(pool_handle, 16, 8) ?? i64(0)
p3 = allocator_alloc(pool_handle, 16, 8) ?? i64(0)
print(p1 != i64(0))
print(p2 != i64(0))
print(p3 == i64(0))
print(allocator_dealloc(pool_handle, p1, 16, 8))
p4 = allocator_alloc(pool_handle, 16, 8) ?? i64(0)
print(p4 != i64(0))
print(allocator_destroy(pool_handle))
EOF
assert_runs "allocator_results" "$TMP_DIR/allocator.tiq" "true
0
true
0
true
0
true
true
true
0
true
0"

# M16.4: std/dl.tiq wrappers load a real dynamic library and call
# through the generic integer ABI.
cat > "$TMP_DIR/dltest.c" <<'EOF'
#include <stdint.h>
int64_t dltest_add(int64_t a, int64_t b) { return a + b; }
EOF
if [ "$(uname)" = "Darwin" ]; then
  "$CC_BIN" -std=c11 -dynamiclib "$TMP_DIR/dltest.c" -o "$TMP_DIR/libdltest.dylib"
  DL_LIB="$TMP_DIR/libdltest.dylib"
else
  "$CC_BIN" -std=c11 -shared -fPIC "$TMP_DIR/dltest.c" -o "$TMP_DIR/libdltest.so"
  DL_LIB="$TMP_DIR/libdltest.so"
fi
cat > "$TMP_DIR/dl_wrapper.tiq" <<EOF
import "std/dl.tiq"
h <- dl_open("$DL_LIB")
s <- dl_sym(h, "dltest_add")
print(dl_call(s, 20, 22, 0, 0, 0, 0))
EOF
assert_runs "dl_wrapper_results" "$TMP_DIR/dl_wrapper.tiq" "42"

# --- 6. ASan/UBSan on a program using std/ imports ---

cat > "$TMP_DIR/asan.tiq" <<'EOF'
import "std/json.tiq"
s <- json_encode_str("hello \"world\"")
print(s)
n <- json_parse_int("123")
print(n)
EOF
exe_file="$TMP_DIR/asan.bin"
err_file="$TMP_DIR/asan.err"
if ! "$TIQ" build "$TMP_DIR/asan.tiq" -o "$exe_file" --asan 2>"$err_file"; then
  fail "asan_build" "$err_file"
fi
out_file="$TMP_DIR/asan.out"
if ! "$exe_file" > "$out_file" 2>"$err_file"; then
  fail "asan_run" "$err_file"
fi
if ! grep -q '"hello \\"world\\""' "$out_file"; then
  fail "asan_output" "$out_file"
fi

echo "std_mod: all tests passed"
