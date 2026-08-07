#!/bin/sh
# M17.4: wasm32-wasi backend tests.
#
# 1. Golden equivalence: for examples the IR supports, wasm output must match
#    the C backend byte-for-byte (print writes a trailing newline, per
#    LANGUAGE_SPEC §12).
# 2. Module validation: built wasm must have the \0asm magic + MVP version.
# 3. Fail-closed: unsupported input (structs, stream generators) must be a
#    compile-time diagnostic with a source location, never a garbage module.
#
# Execution requires `node` (WASI preview1). If node is absent the validation
# and fail-closed checks still run; the golden-equivalence section is skipped.
set -u

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-wasm-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail=0

# Node runner for wasm modules; writes stdout only. On runtime failure prints
# to stderr and exits nonzero.
WRAPPER="$TMP_DIR/run_wasm.js"
cat > "$WRAPPER" <<'EOF'
const { WASI } = require("wasi");
const fs = require("fs");
const wasi = new WASI({ args: [], env: {}, version: "preview1" });
const bytes = fs.readFileSync(process.argv[2]);
WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi.wasiImport })
  .then(({ instance }) => { wasi.start(instance); })
  .catch((e) => { console.error("wasm runtime error: " + e.message); process.exit(1); });
EOF

# --- Test 1: golden equivalence with the C backend -------------------------

node_available=0
if command -v node >/dev/null 2>&1; then node_available=1; fi

golden_files="examples/hello.tiq
examples/max.tiq
examples/count.tiq
examples/gcd.tiq
examples/primes.tiq
examples/continue_skip.tiq
examples/fib.tiq
examples/arithmetic.tiq"

for src in $golden_files; do
  name=$(basename "$src" .tiq)

  if ! $TIQ build "$src" -o "$TMP_DIR/$name.c" 2>"$TMP_DIR/$name.c.err"; then
    echo "wasm: FAIL $src (C backend build error)" >&2
    cat "$TMP_DIR/$name.c.err" >&2
    fail=1
    continue
  fi
  c_out=$("$TMP_DIR/$name.c" 2>/dev/null)

  if ! $TIQ build "$src" --target wasm32-wasi -o "$TMP_DIR/$name.wasm" 2>"$TMP_DIR/$name.wasm.err"; then
    echo "wasm: FAIL $src (wasm build error)" >&2
    cat "$TMP_DIR/$name.wasm.err" >&2
    fail=1
    continue
  fi

  # Magic bytes: \0asm, version 1 (little-endian)
  magic=$(od -An -tx1 -N8 "$TMP_DIR/$name.wasm" 2>/dev/null | tr -d ' \n')
  if [ "$magic" != "0061736d01000000" ]; then
    echo "wasm: FAIL $src (bad module header: $magic)" >&2
    fail=1
  fi

  if [ "$node_available" -eq 1 ]; then
    wasm_out=$(node "$WRAPPER" "$TMP_DIR/$name.wasm" 2>"$TMP_DIR/$name.wasm.run.err")
    if [ $? -ne 0 ]; then
      echo "wasm: FAIL $src (runtime error)" >&2
      cat "$TMP_DIR/$name.wasm.run.err" >&2
      fail=1
      continue
    fi
    if [ "$c_out" != "$wasm_out" ]; then
      echo "wasm: FAIL $src (output mismatch)" >&2
      echo "  C backend:   $(printf '%s' "$c_out" | od -c | head -5)" >&2
      echo "  wasm output: $(printf '%s' "$wasm_out" | od -c | head -5)" >&2
      fail=1
    fi
  fi
done

# --- Test 2: fail-closed on unsupported input ------------------------------

# Structs: IR lowering does not support field access / record literals.
# These must be a clean compile error with a source location, not a garbage
# module or a silent miscompile.
fail_closed_files="examples/structs.tiq"

for src in $fail_closed_files; do
  name=$(basename "$src" .tiq)
  if $TIQ build "$src" --target wasm32-wasi -o "$TMP_DIR/$name.wasm" 2>"$TMP_DIR/$name.err"; then
    echo "wasm: FAIL $src (unsupported input must not build)" >&2
    fail=1
  else
    if ! grep -qE ':[0-9]+:' "$TMP_DIR/$name.err"; then
      echo "wasm: FAIL $src (diagnostic lacks source location)" >&2
      cat "$TMP_DIR/$name.err" >&2
      fail=1
    fi
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "wasm: failed" >&2
  exit 1
fi
echo "wasm: ok"
