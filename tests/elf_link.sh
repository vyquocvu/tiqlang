#!/bin/sh
# M17.3.3: integrated ELF64 executable linker tests (aarch64 Linux host).
#
# Verifies that `tiq link-qbe` links ELF relocatable objects into a
# runnable ELF executable without invoking cc/ld, and that `tiq build
# --backend qbe` produces identical behavior through the integrated
# pipeline. Functional goldens: end-to-end programs run with expected
# stdout/exit codes; determinism: repeated links are byte-identical.
# On non-Linux-aarch64 hosts the suite is skipped.
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-elf-link-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

HOST="$(uname -s)-$(uname -m)"
if [ "$HOST" != "Linux-aarch64" ]; then
  echo "elf_link: skipped (host is $HOST, integrated ELF linker is Linux-aarch64 only)"
  exit 0
fi

if [ ! -x "$TIQ" ] || [ ! -x ./build/qbe ] || [ ! -f ./build/runtime_qbe.o ]; then
  echo "elf_link: missing build artifacts (tiq, qbe, runtime_qbe.o)" >&2
  exit 1
fi

# Compile a Tiq source through the integrated pipeline:
# source -> QBE IL -> qbe .s -> emit-obj .o -> link-qbe executable.
build_integrated() {
  local src="$1"
  local exe="$2"
  local base="$TMP_DIR/$(basename "$src" .tiq)"
  $TIQ dump-qbe "$src" > "$base.il" 2>"$base.il.err" || {
    echo "build_integrated: dump-qbe failed for $src" >&2
    cat "$base.il.err" >&2
    return 1
  }
  ./build/qbe -o "$base.s" "$base.il" || {
    echo "build_integrated: qbe failed for $src" >&2
    return 1
  }
  $TIQ emit-obj "$base.s" -o "$base.o" 2>"$base.o.err" || {
    echo "build_integrated: emit-obj failed for $src" >&2
    cat "$base.o.err" >&2
    return 1
  }
  $TIQ link-qbe "$base.o" ./build/runtime_qbe.o -o "$exe" 2>"$base.link.err" || {
    echo "build_integrated: link-qbe failed for $src" >&2
    cat "$base.link.err" >&2
    return 1
  }
}

expect_same() {
  local name="$1" src="$2" exe="$3"
  build_integrated "$src" "$exe"
  local got want
  got="$("$exe")"
  want="$($TIQ run "$src")"
  if [ "$got" != "$want" ]; then
    echo "$name: integrated output differs from C backend" >&2
    printf 'got:  %s\nwant: %s\n' "$got" "$want" >&2
    exit 1
  fi
}

# --- Test 1: usage fail-closed ----------------------------------------------

if $TIQ link-qbe 2>"$TMP_DIR/usage.err"; then
  echo "usage: link-qbe with no args must fail" >&2
  exit 1
fi
rc=0; $TIQ link-qbe >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "usage: expected exit 2, got $rc" >&2
  exit 1
fi
if ! grep -q "link-qbe" "$TMP_DIR/usage.err"; then
  echo "usage: stderr must mention link-qbe" >&2
  exit 1
fi
rc=0; $TIQ link-qbe a.o --bogus -o x >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "usage: unknown option must exit 2, got $rc" >&2
  exit 1
fi
rc=0; $TIQ link-qbe a.o 2>/dev/null || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "usage: missing -o must exit 2, got $rc" >&2
  exit 1
fi

# --- Test 2: missing input fails closed -------------------------------------

rc=0; $TIQ link-qbe "$TMP_DIR/does-not-exist.o" ./build/runtime_qbe.o -o "$TMP_DIR/x" 2>"$TMP_DIR/miss.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "missing: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -q "cannot read" "$TMP_DIR/miss.err"; then
  echo "missing: expected 'cannot read' diagnostic" >&2
  cat "$TMP_DIR/miss.err" >&2
  exit 1
fi

# --- Test 3: non-ELF input fails closed -------------------------------------

printf 'not an elf file at all\n' > "$TMP_DIR/garbage.o"
rc=0; $TIQ link-qbe "$TMP_DIR/garbage.o" ./build/runtime_qbe.o -o "$TMP_DIR/x" 2>"$TMP_DIR/garb.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "garbage: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -qi "elf" "$TMP_DIR/garb.err"; then
  echo "garbage: expected an ELF diagnostic" >&2
  cat "$TMP_DIR/garb.err" >&2
  exit 1
fi

# --- Test 4: end-to-end equivalence with the C backend -----------------------

expect_same hello examples/hello.tiq "$TMP_DIR/hello"
expect_same count examples/count.tiq "$TMP_DIR/count"
expect_same gcd examples/gcd.tiq "$TMP_DIR/gcd"
expect_same max examples/max.tiq "$TMP_DIR/max"

# --- Test 5: exit code propagation -------------------------------------------

"$TMP_DIR/hello" >/dev/null
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "exit: hello must exit 0, got $rc" >&2
  exit 1
fi

# --- Test 6: deterministic output --------------------------------------------

build_integrated examples/hello.tiq "$TMP_DIR/hello2"
if ! cmp -s "$TMP_DIR/hello" "$TMP_DIR/hello2"; then
  echo "determinism: two links of identical inputs must be byte-identical" >&2
  exit 1
fi

# --- Test 7: readelf structural validation (Linux only) ----------------------

if command -v readelf >/dev/null 2>&1; then
  if ! readelf -h "$TMP_DIR/hello" > "$TMP_DIR/readelf.out" 2>/dev/null; then
    echo "readelf: readelf -h failed on hello" >&2
    exit 1
  fi
  if ! grep -q "AArch64" "$TMP_DIR/readelf.out"; then
    echo "readelf: expected AArch64 machine" >&2
    exit 1
  fi
  if ! grep -q "EXEC" "$TMP_DIR/readelf.out"; then
    echo "readelf: expected EXEC type" >&2
    exit 1
  fi
  # Verify interpreter segment exists
  if ! readelf -l "$TMP_DIR/hello" > "$TMP_DIR/readelf_phdr.out" 2>/dev/null; then
    echo "readelf: readelf -l failed" >&2
    exit 1
  fi
  if ! grep -q "INTERP" "$TMP_DIR/readelf_phdr.out"; then
    echo "readelf: expected INTERP program header" >&2
    exit 1
  fi
fi

# --- Test 8: tiq build --backend qbe uses the integrated linker --------------

$TIQ build examples/hello.tiq --backend qbe -o "$TMP_DIR/hello-be"
OUT="$("$TMP_DIR/hello-be")"
WANT="$($TIQ run examples/hello.tiq)"
if [ "$OUT" != "$WANT" ]; then
  echo "backend: qbe build output differs from C backend" >&2
  exit 1
fi

# --- Test 9: TIQ_QBE_LINK=cc escape hatch ------------------------------------

TIQ_QBE_LINK=cc $TIQ build examples/hello.tiq --backend qbe -o "$TMP_DIR/hello-cc"
OUT="$("$TMP_DIR/hello-cc")"
if [ "$OUT" != "$WANT" ]; then
  echo "escape: TIQ_QBE_LINK=cc build output differs from C backend" >&2
  exit 1
fi

# --- Test 10: no leftover temp files ------------------------------------------

BEFORE="$(ls "${TMPDIR:-/tmp}" | grep -c 'tiq-qbe-' || true)"
build_integrated examples/gcd.tiq "$TMP_DIR/gcd3"
AFTER="$(ls "${TMPDIR:-/tmp}" | grep -c 'tiq-qbe-' || true)"
if [ "$AFTER" -ne "$BEFORE" ]; then
  echo "tempfiles: integrated pipeline must not leak tiq-qbe-* files" >&2
  exit 1
fi

echo "ELF executable linker tests passed"
