#!/bin/sh
# M13.1-P6: module system (`import`) — multi-file fixtures exercising the
# DFS loader: flat namespace, post-order execution, canonical-path dedupe,
# cycle detection, position rule, and deterministic multi-module emission
# (LANGUAGE_SPEC §17.6, GRAMMAR import_decl).
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-module-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "module: FAIL $1" >&2
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
  if [ -e "$exe_file" ]; then
    fail "$name (unexpectedly produced an executable)"
  fi
  if ! grep -q "$pattern" "$err_file"; then
    echo "module: FAIL $name (diagnostic mismatch)" >&2
    echo "expected pattern: $pattern" >&2
    echo "actual:" >&2
    cat "$err_file" >&2
    exit 1
  fi
}

# 1. basic: main imports lib; lib function, enum, and struct are visible.
mkdir -p "$TMP_DIR/basic"
cat > "$TMP_DIR/basic/lib.tiq" << 'EOF'
enum Color { Red, Green, Blue }
struct Point {
    x: i64,
    y: i64
}
twice n -> n * 2
EOF
cat > "$TMP_DIR/basic/main.tiq" << 'EOF'
import "lib.tiq"
print(twice(21))
print(Color.Green)
p = Point { x: 3, y: 4 }
print(p.x + p.y)
EOF
assert_runs "basic" "$TMP_DIR/basic/main.tiq" '42
1
7'

# 2. chain: a imports b imports c — c's symbols visible in a; imported
# top-level statements run first (post-order: c, b, a).
mkdir -p "$TMP_DIR/chain"
cat > "$TMP_DIR/chain/c.tiq" << 'EOF'
cval = 7
EOF
cat > "$TMP_DIR/chain/b.tiq" << 'EOF'
import "c.tiq"
bval = cval + 1
EOF
cat > "$TMP_DIR/chain/a.tiq" << 'EOF'
import "b.tiq"
print(cval)
print(bval)
EOF
assert_runs "chain" "$TMP_DIR/chain/a.tiq" '7
8'

# 3. diamond dedupe: a imports b and c, both import d via different
# relative spellings; if d were loaded twice, its enum would re-register
# and E24 (duplicate enum definition) would fire — a passing run proves
# the canonical-path dedupe.
mkdir -p "$TMP_DIR/diamond/sub"
cat > "$TMP_DIR/diamond/sub/d.tiq" << 'EOF'
enum Dup { A }
dval = 10
EOF
cat > "$TMP_DIR/diamond/b.tiq" << 'EOF'
import "sub/d.tiq"
bval = dval + 1
EOF
cat > "$TMP_DIR/diamond/c.tiq" << 'EOF'
import "./sub/d.tiq"
cval = dval + 2
EOF
cat > "$TMP_DIR/diamond/a.tiq" << 'EOF'
import "b.tiq"
import "c.tiq"
print(bval + cval)
print(Dup.A)
EOF
assert_runs "diamond" "$TMP_DIR/diamond/a.tiq" '23
0'

# 4. not-found: E27 with the path as written + location of the import.
mkdir -p "$TMP_DIR/notfound"
cat > "$TMP_DIR/notfound/main.tiq" << 'EOF'
import "missing.tiq"
print(1)
EOF
assert_rejects "notfound" "$TMP_DIR/notfound/main.tiq" \
  'main\.tiq:1: error\[E27\]: module not found: "missing.tiq"'

# 5. circular: a imports b imports a — E28 with the cycle chain.
mkdir -p "$TMP_DIR/circ"
cat > "$TMP_DIR/circ/a.tiq" << 'EOF'
import "b.tiq"
print(1)
EOF
cat > "$TMP_DIR/circ/b.tiq" << 'EOF'
import "a.tiq"
print(2)
EOF
assert_rejects "circular" "$TMP_DIR/circ/a.tiq" \
  'b\.tiq:1: error\[E28\]: circular import: .*a\.tiq -> .*b\.tiq -> .*a\.tiq'

# 6. import below a non-import item: position rule (E04).
mkdir -p "$TMP_DIR/pos"
cat > "$TMP_DIR/pos/lib.tiq" << 'EOF'
x = 1
EOF
cat > "$TMP_DIR/pos/main.tiq" << 'EOF'
y = 2
import "lib.tiq"
EOF
assert_rejects "import_after_item" "$TMP_DIR/pos/main.tiq" \
  'main\.tiq:2: error\[E04\]: import must appear before any other top-level item'

# 6b. import operand must be a string literal (E04).
mkdir -p "$TMP_DIR/nonstr"
cat > "$TMP_DIR/nonstr/main.tiq" << 'EOF'
import lib
EOF
assert_rejects "import_non_string" "$TMP_DIR/nonstr/main.tiq" \
  "error\[E04\]: expected string literal path after 'import'"

# 7. duplicate top-level definition across modules reuses the existing
# duplicate diagnostic (E24 for enums).
mkdir -p "$TMP_DIR/dup"
cat > "$TMP_DIR/dup/lib.tiq" << 'EOF'
enum Shade { Light }
EOF
cat > "$TMP_DIR/dup/main.tiq" << 'EOF'
import "lib.tiq"
enum Shade { Dark }
print(1)
EOF
assert_rejects "duplicate_across_modules" "$TMP_DIR/dup/main.tiq" \
  "error\[E24\]: duplicate enum definition 'Shade'"

# 8. determinism: emit-c a multi-module program twice — byte-identical,
# and the generated C contains no filesystem paths from the fixture dir.
if ! "$TIQ" emit-c "$TMP_DIR/diamond/a.tiq" > "$TMP_DIR/det.1.c" 2>"$TMP_DIR/det.err"; then
  fail "determinism (emit-c error)" "$TMP_DIR/det.err"
fi
if ! "$TIQ" emit-c "$TMP_DIR/diamond/a.tiq" > "$TMP_DIR/det.2.c" 2>"$TMP_DIR/det.err"; then
  fail "determinism (emit-c error on second run)" "$TMP_DIR/det.err"
fi
if ! cmp -s "$TMP_DIR/det.1.c" "$TMP_DIR/det.2.c"; then
  fail "determinism (multi-module emit-c output differs between runs)"
fi
if grep -qF "$TMP_DIR" "$TMP_DIR/det.1.c"; then
  fail "determinism (generated C leaks filesystem paths)"
fi

echo "module: ok"
