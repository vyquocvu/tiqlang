#!/bin/sh
# M17.3.3: integrated ELF64 object writer tests (aarch64 Linux host).
#
# Verifies that `tiq emit-obj` assembles the QBE arm64 assembly subset
# in ELF mode and writes a valid ELF64 relocatable object, and that
# `tiq build --backend qbe` no longer needs the external `cc -c`
# assembler step. Goldens are functional (assemble + link with system
# linker on Linux) plus structural byte pins.
# On non-Linux-aarch64 hosts the suite is skipped: the integrated ELF
# writer is host-format scoped.
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-elf-obj-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

HOST="$(uname -s)-$(uname -m)"
if [ "$HOST" != "Linux-aarch64" ]; then
  echo "elf_obj: skipped (host is $HOST, integrated ELF writer is Linux-aarch64 only)"
  exit 0
fi

if [ ! -x "$TIQ" ]; then
  echo "elf_obj: missing $TIQ" >&2
  exit 1
fi

# Assemble a .s fixture into a runnable executable via the integrated
# object writer + host linker, then capture its exit status.
assemble_and_link() {
  local src="$1" obj="$2" exe="$3"
  if ! $TIQ emit-obj "$src" -o "$obj" 2>"$TMP_DIR/asm.err"; then
    echo "assemble_and_link: emit-obj failed for $src" >&2
    cat "$TMP_DIR/asm.err" >&2
    return 1
  fi
  if ! cc "$obj" -o "$exe" 2>"$TMP_DIR/link.err"; then
    echo "assemble_and_link: host link failed for $src" >&2
    cat "$TMP_DIR/link.err" >&2
    return 1
  fi
}

expect_exit() {
  local name="$1" src="$2" want="$3"
  local obj="$TMP_DIR/${name}.o" exe="$TMP_DIR/${name}" got=0
  assemble_and_link "$src" "$obj" "$exe" || exit 1
  "$exe" || got=$?
  if [ "$got" -ne "$want" ]; then
    echo "$name: expected exit $want, got $got" >&2
    exit 1
  fi
}

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
if ! grep -q "emit-obj" "$TMP_DIR/usage.err"; then
  echo "usage: stderr must mention emit-obj" >&2
  exit 1
fi
rc=0; $TIQ emit-obj "$TMP_DIR/nope.s" --bogus -o "$TMP_DIR/nope.o" >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "usage: unknown option must exit 2, got $rc" >&2
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

# --- Test 3: unsupported input fails closed with location -------------------

printf '\tnop\n' > "$TMP_DIR/nop.s"
rc=0; $TIQ emit-obj "$TMP_DIR/nop.s" -o "$TMP_DIR/nop.o" 2>"$TMP_DIR/nop.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "unsupported: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -q "nop.s:1: error" "$TMP_DIR/nop.err"; then
  echo "unsupported: expected located diagnostic 'nop.s:1: error'" >&2
  cat "$TMP_DIR/nop.err" >&2
  exit 1
fi

# --- Test 4: mov immediate + external bl (exit code via exit) --------------

printf '.text\n.globl main\nmain:\n\tmov\tx0, #42\n\tbl\texit\n' > "$TMP_DIR/exit42.s"
expect_exit exit42 "$TMP_DIR/exit42.s" 42

# --- Test 5: frame, loop, compare, conditional branch -----------------------

cat > "$TMP_DIR/loop.s" <<'EOF'
.text
.globl main
main:
	stp	x29, x30, [sp, -16]!
	mov	x29, sp
	mov	x19, #0
	mov	x20, #1
.L1:
	cmp	x20, #6
	bge	.L2
	add	x19, x19, x20
	add	x20, x20, #1
	b	.L1
.L2:
	mov	x0, x19
	ldp	x29, x30, [sp], 16
	bl	exit
EOF
expect_exit loop "$TMP_DIR/loop.s" 15

# --- Test 6: internal function call via bl + ret ----------------------------

cat > "$TMP_DIR/call.s" <<'EOF'
.text
.globl main
main:
	bl	_helper
	bl	exit
_helper:
	mov	x0, #7
	ret
EOF
expect_exit call "$TMP_DIR/call.s" 7

# --- Test 7: ELF structural bytes -----------------------------------------

if ! $TIQ emit-obj "$TMP_DIR/exit42.s" -o "$TMP_DIR/struct.o" 2>/dev/null; then
  echo "struct: emit-obj failed" >&2
  exit 1
fi
# ELF magic: 7f 45 4c 46
magic=$(od -An -tx1 -N4 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$magic" != "7f454c46" ]; then
  echo "struct: bad ELF magic: $magic" >&2
  exit 1
fi
# EI_CLASS = ELFCLASS64 (2) at offset 4
eiclass=$(od -An -tx1 -j4 -N1 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$eiclass" != "02" ]; then
  echo "struct: bad EI_CLASS: $eiclass" >&2
  exit 1
fi
# e_type = ET_REL (1) at offset 16 (little endian: 01 00)
etype=$(od -An -tx1 -j16 -N2 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$etype" != "0100" ]; then
  echo "struct: bad e_type: $etype" >&2
  exit 1
fi
# e_machine = EM_AARCH64 (183 = 0x00B7) at offset 18 (little endian: B7 00)
emachine=$(od -An -tx1 -j18 -N2 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$emachine" != "b700" ]; then
  echo "struct: bad e_machine: $emachine" >&2
  exit 1
fi

# --- Test 8: deterministic emission -----------------------------------------

$TIQ emit-obj "$TMP_DIR/loop.s" -o "$TMP_DIR/det1.o"
$TIQ emit-obj "$TMP_DIR/loop.s" -o "$TMP_DIR/det2.o"
if ! cmp -s "$TMP_DIR/det1.o" "$TMP_DIR/det2.o"; then
  echo "determinism: repeated emission differs" >&2
  exit 1
fi

# --- Test 9: readelf structural validation (Linux only) ---------------------

if command -v readelf >/dev/null 2>&1; then
  if ! readelf -h "$TMP_DIR/struct.o" > "$TMP_DIR/readelf.out" 2>/dev/null; then
    echo "readelf: readelf -h failed on struct.o" >&2
    exit 1
  fi
  if ! grep -q "AArch64" "$TMP_DIR/readelf.out"; then
    echo "readelf: expected AArch64 machine" >&2
    exit 1
  fi
  if ! grep -q "REL" "$TMP_DIR/readelf.out"; then
    echo "readelf: expected REL type" >&2
    exit 1
  fi
fi

# --- Test 10: qbe backend end-to-end uses the integrated writer --------------

printf 'print("hello")\n' > "$TMP_DIR/hello.tiq"
if ! $TIQ build "$TMP_DIR/hello.tiq" --backend qbe -o "$TMP_DIR/hello1" 2>"$TMP_DIR/hello.err"; then
  echo "e2e: qbe build failed" >&2
  cat "$TMP_DIR/hello.err" >&2
  exit 1
fi
out=$("$TMP_DIR/hello1")
if [ "$out" != "hello" ]; then
  echo "e2e: expected 'hello', got '$out'" >&2
  exit 1
fi

echo "ELF object writer tests passed"
