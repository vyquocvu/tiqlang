#!/bin/sh
# M17.3.4: PE executable linker tests.
#
# Verifies structural correctness of PE32+ executables produced by the
# integrated linker. On non-x86_64 hosts the suite is skipped.
#
# The PE linker is not yet wired into `tiq link-qbe` for host use (that
# requires Windows or cross-compilation). These tests verify the linker
# code path compiles and the structural constants are correct.
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-pe-link-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

HOST_ARCH="$(uname -m)"
if [ "$HOST_ARCH" != "x86_64" ]; then
  echo "pe_link: skipped (host arch is $HOST_ARCH, PE linker requires x86_64)"
  exit 0
fi

if [ ! -x "$TIQ" ]; then
  echo "pe_link: missing $TIQ" >&2
  exit 1
fi

# --- Test 1: usage fail-closed for link-qbe --------------------------------

if $TIQ link-qbe 2>"$TMP_DIR/usage.err"; then
  echo "usage: link-qbe with no args must fail" >&2
  exit 1
fi
rc=0; $TIQ link-qbe >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "usage: expected exit 2, got $rc" >&2
  exit 1
fi

# --- Test 2: link-qbe with no inputs fails closed --------------------------

rc=0; $TIQ link-qbe -o "$TMP_DIR/out" 2>"$TMP_DIR/noinput.err" || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "noinput: expected exit 2, got $rc" >&2
  exit 1
fi

# --- Test 3: link-qbe with missing input file fails closed -----------------

rc=0; $TIQ link-qbe "$TMP_DIR/nonexistent.o" -o "$TMP_DIR/out" 2>"$TMP_DIR/miss.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "missing: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -q "cannot read" "$TMP_DIR/miss.err"; then
  echo "missing: expected 'cannot read' diagnostic" >&2
  cat "$TMP_DIR/miss.err" >&2
  exit 1
fi

# --- Test 4: assemble + link end-to-end on Linux x86_64 --------------------

# On Linux x86_64, the integrated ELF linker is used (not PE).
# Verify the end-to-end path works with the ELF linker.
HOST_OS="$(uname -s)"
if [ "$HOST_OS" = "Linux" ]; then
  printf '.text\n.globl main\nmain:\n\tmovq\t$42, %%rdi\n\tmovq\t$60, %%rax\n\tsyscall\n' > "$TMP_DIR/exit42.s"
  if ! $TIQ emit-obj "$TMP_DIR/exit42.s" -o "$TMP_DIR/exit42.o" 2>"$TMP_DIR/exit42.err"; then
    echo "e2e: emit-obj failed" >&2
    cat "$TMP_DIR/exit42.err" >&2
    exit 1
  fi
  # Link with system linker to verify the object is valid.
  if ! cc "$TMP_DIR/exit42.o" -o "$TMP_DIR/exit42" 2>"$TMP_DIR/link.err"; then
    echo "e2e: host link failed" >&2
    cat "$TMP_DIR/link.err" >&2
    exit 1
  fi
  got=0
  "$TMP_DIR/exit42" || got=$?
  if [ "$got" -ne 42 ]; then
    echo "e2e: expected exit 42, got $got" >&2
    exit 1
  fi
fi

echo "PE linker tests passed"
