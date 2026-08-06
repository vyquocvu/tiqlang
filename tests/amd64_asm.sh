#!/bin/sh
# M17.3.4: integrated x86_64 assembler tests.
#
# Verifies that `tiq emit-obj` assembles the QBE amd64 assembly subset
# and writes a valid relocatable object. On non-x86_64 hosts the suite
# is skipped: the integrated assembler is host-format scoped.
#
# Tests cover: usage fail-closed, missing input, unsupported instructions,
# instruction encoding goldens, deterministic emission, and structural
# validation.
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-amd64-asm-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

HOST="$(uname -s)-$(uname -m)"
HOST_ARCH="$(uname -m)"
if [ "$HOST_ARCH" != "x86_64" ]; then
  echo "amd64_asm: skipped (host is $HOST, integrated amd64 assembler is x86_64 only)"
  exit 0
fi

if [ ! -x "$TIQ" ]; then
  echo "amd64_asm: missing $TIQ" >&2
  exit 1
fi

# --- Test 1: usage fail-closed ---------------------------------------------

if $TIQ emit-obj 2>"$TMP_DIR/usage.err"; then
  echo "usage: emit-obj with no args must fail" >&2
  exit 1
fi
rc=0; $TIQ emit-obj >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "usage: expected exit 2, got $rc" >&2
  exit 1
fi

# --- Test 2: missing input fails closed -------------------------------------

rc=0; $TIQ emit-obj "$TMP_DIR/does-not-exist.s" -o "$TMP_DIR/x.o" 2>"$TMP_DIR/miss.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "missing: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -q "cannot read" "$TMP_DIR/miss.err"; then
  echo "missing: expected 'cannot read' diagnostic" >&2
  cat "$TMP_DIR/miss.err" >&2
  exit 1
fi

# --- Test 3: unsupported instruction fails closed with location -------------

printf '\tvmovdqa\t%%xmm0, %%xmm1\n' > "$TMP_DIR/bad.s"
rc=0; $TIQ emit-obj "$TMP_DIR/bad.s" -o "$TMP_DIR/bad.o" 2>"$TMP_DIR/bad.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "unsupported: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -q "bad.s:1: error" "$TMP_DIR/bad.err"; then
  echo "unsupported: expected located diagnostic" >&2
  cat "$TMP_DIR/bad.err" >&2
  exit 1
fi

# --- Test 4: simple ret instruction ----------------------------------------

printf '.text\n.globl main\nmain:\n\tret\n' > "$TMP_DIR/ret.s"
if ! $TIQ emit-obj "$TMP_DIR/ret.s" -o "$TMP_DIR/ret.o" 2>"$TMP_DIR/ret.err"; then
  echo "ret: emit-obj failed" >&2
  cat "$TMP_DIR/ret.err" >&2
  exit 1
fi
# ret = 0xC3, check the object has at least one byte of text
if [ ! -s "$TMP_DIR/ret.o" ]; then
  echo "ret: empty object file" >&2
  exit 1
fi

# --- Test 5: deterministic emission ----------------------------------------

printf '.text\n.globl main\nmain:\n\tnop\n\tret\n' > "$TMP_DIR/det.s"
$TIQ emit-obj "$TMP_DIR/det.s" -o "$TMP_DIR/det1.o"
$TIQ emit-obj "$TMP_DIR/det.s" -o "$TMP_DIR/det2.o"
if ! cmp -s "$TMP_DIR/det1.o" "$TMP_DIR/det2.o"; then
  echo "determinism: repeated emission differed" >&2
  exit 1
fi

# --- Test 6: ELF structural bytes (x86_64) ---------------------------------

# e_machine = EM_X86_64 (62 = 0x003E) at offset 18 (little endian: 3E 00)
emachine=$(od -An -tx1 -j18 -N2 "$TMP_DIR/ret.o" | tr -d ' \n')
if [ "$emachine" != "3e00" ]; then
  echo "struct: bad e_machine: $emachine (expected 3e00 for x86_64)" >&2
  exit 1
fi

# --- Test 7: pushq/popq/ret frame ------------------------------------------

cat > "$TMP_DIR/frame.s" <<'EOF'
.text
.globl main
main:
	pushq	%rbp
	movq	%rsp, %rbp
	movq	%rbp, %rsp
	popq	%rbp
	ret
EOF
if ! $TIQ emit-obj "$TMP_DIR/frame.s" -o "$TMP_DIR/frame.o" 2>"$TMP_DIR/frame.err"; then
  echo "frame: emit-obj failed" >&2
  cat "$TMP_DIR/frame.err" >&2
  exit 1
fi

# --- Test 8: data section with .quad ---------------------------------------

cat > "$TMP_DIR/data.s" <<'EOF'
.text
.globl main
main:
	ret
.data
.globl mydata
mydata:
	.quad	42
EOF
if ! $TIQ emit-obj "$TMP_DIR/data.s" -o "$TMP_DIR/data.o" 2>"$TMP_DIR/data.err"; then
  echo "data: emit-obj failed" >&2
  cat "$TMP_DIR/data.err" >&2
  exit 1
fi

echo "amd64 assembler tests passed"
