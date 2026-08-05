#!/bin/sh
# M17.3.1: integrated Mach-O object writer tests (arm64 macOS host).
#
# Verifies that `tiq emit-obj` assembles the QBE arm64 assembly subset
# in-process and writes a valid Mach-O relocatable object, and that
# `tiq build --backend qbe` no longer needs the external `cc -c`
# assembler step. Goldens are functional (link the object with the host
# linker, run it, check the exit status) plus structural byte pins.
# On non-Darwin-arm64 hosts the suite is skipped: the integrated writer
# is host-format scoped (M17.3.3 covers ELF).
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-macho-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

HOST="$(uname -s)-$(uname -m)"
if [ "$HOST" != "Darwin-arm64" ]; then
  echo "macho_obj: skipped (host is $HOST, integrated Mach-O writer is Darwin-arm64 only)"
  exit 0
fi

if [ ! -x "$TIQ" ]; then
  echo "macho_obj: missing $TIQ" >&2
  exit 1
fi

# Assemble a .s fixture into a runnable executable via the integrated
# object writer + host linker, then capture its exit status.
# Usage: run_exit_code <fixture.s> <varname>
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
if [ -e "$TMP_DIR/x.o" ]; then
  echo "missing: no output file may be written on failure" >&2
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
if [ -e "$TMP_DIR/nop.o" ]; then
  echo "unsupported: no output file may be written on failure" >&2
  exit 1
fi
printf '.section .weird\n' > "$TMP_DIR/sec.s"
rc=0; $TIQ emit-obj "$TMP_DIR/sec.s" -o "$TMP_DIR/sec.o" 2>"$TMP_DIR/sec.err" || rc=$?
if [ "$rc" -ne 1 ] || ! grep -q "sec.s:1: error" "$TMP_DIR/sec.err"; then
  echo "unsupported: .section must fail closed with location" >&2
  cat "$TMP_DIR/sec.err" >&2
  exit 1
fi
printf '.text\nL1:\n\tb\tL9\n' > "$TMP_DIR/undef.s"
rc=0; $TIQ emit-obj "$TMP_DIR/undef.s" -o "$TMP_DIR/undef.o" 2>"$TMP_DIR/undef.err" || rc=$?
if [ "$rc" -ne 1 ] || ! grep -q "undef.s:3: error" "$TMP_DIR/undef.err"; then
  echo "unsupported: undefined branch target must fail closed at the branch line" >&2
  cat "$TMP_DIR/undef.err" >&2
  exit 1
fi

# --- Test 4: mov immediate + external bl (exit code via _exit) --------------

printf '.text\n.globl _main\n_main:\n\tmov\tx0, #42\n\tbl\t_exit\n' > "$TMP_DIR/exit42.s"
expect_exit exit42 "$TMP_DIR/exit42.s" 42

# --- Test 5: frame, loop, compare, conditional branch -----------------------

cat > "$TMP_DIR/loop.s" <<'EOF'
.text
.globl _main
_main:
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
	bl	_exit
EOF
expect_exit loop "$TMP_DIR/loop.s" 15

# --- Test 6: internal function call via bl + ret ----------------------------

cat > "$TMP_DIR/call.s" <<'EOF'
.text
.globl _main
_main:
	bl	_helper
	bl	_exit
_helper:
	mov	x0, #7
	ret
EOF
expect_exit call "$TMP_DIR/call.s" 7

# --- Test 7: stack slot store/load ------------------------------------------

cat > "$TMP_DIR/slot.s" <<'EOF'
.text
.globl _main
_main:
	stp	x29, x30, [sp, -16]!
	mov	x29, sp
	mov	x9, #20
	str	x9, [x29, 16]
	ldr	x9, [x29, 16]
	add	x9, x9, #22
	mov	x0, x9
	bl	_exit
EOF
expect_exit slot "$TMP_DIR/slot.s" 42

# --- Test 8: data section via adrp/add relocations --------------------------

cat > "$TMP_DIR/data.s" <<'EOF'
.data
.balign 8
_.c:
	.ascii "A"
	.byte 0
.text
.globl _main
_main:
	adrp	x9, _.c@PAGE
	add	x9, x9, #:lo12:_.c@PAGEOFF
	ldrb	w9, [x9]
	mov	x0, x9
	bl	_exit
EOF
expect_exit data "$TMP_DIR/data.s" 65

# --- Test 9: Mach-O structural bytes -----------------------------------------

if ! $TIQ emit-obj "$TMP_DIR/exit42.s" -o "$TMP_DIR/struct.o" 2>/dev/null; then
  echo "struct: emit-obj failed" >&2
  exit 1
fi
# magic 0xFEEDFACF (little endian: CF FA ED FE)
magic=$(od -An -tx1 -N4 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$magic" != "cffaedfe" ]; then
  echo "struct: bad Mach-O magic: $magic" >&2
  exit 1
fi
# cputype CPU_TYPE_ARM64 = 0x0100000C (little endian: 0C 00 00 01)
cpu=$(od -An -tx1 -j4 -N4 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$cpu" != "0c000001" ]; then
  echo "struct: bad cputype: $cpu" >&2
  exit 1
fi
# filetype MH_OBJECT = 1 (offset 12, little endian: 01 00 00 00)
ftype=$(od -An -tx1 -j12 -N4 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$ftype" != "01000000" ]; then
  echo "struct: bad filetype: $ftype" >&2
  exit 1
fi

# --- Test 10: deterministic emission -----------------------------------------

$TIQ emit-obj "$TMP_DIR/loop.s" -o "$TMP_DIR/det1.o"
$TIQ emit-obj "$TMP_DIR/loop.s" -o "$TMP_DIR/det2.o"
if ! cmp -s "$TMP_DIR/det1.o" "$TMP_DIR/det2.o"; then
  echo "determinism: repeated emission differs" >&2
  exit 1
fi

# --- Test 11: qbe backend end-to-end uses the integrated writer --------------

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
if ! $TIQ build "$TMP_DIR/hello.tiq" --backend qbe -o "$TMP_DIR/hello2" 2>/dev/null; then
  echo "e2e: second qbe build failed" >&2
  exit 1
fi
out2=$("$TMP_DIR/hello2")
if [ "$out2" != "hello" ]; then
  echo "e2e: second build expected 'hello', got '$out2'" >&2
  exit 1
fi
# Note: executables are not byte-compared — the host linker embeds a random
# LC_UUID. Object-level determinism of the integrated writer is pinned by
# test 10 above.

echo "Mach-O object writer tests passed"
