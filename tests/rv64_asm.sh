#!/bin/sh
# M17.4.1: integrated ELF64 object writer tests (riscv64 Linux host).
#
# Verifies that `tiq emit-obj` assembles the QBE rv64 assembly subset in
# ELF mode and writes a valid ELF64 relocatable object, and that
# `tiq build --backend qbe` no longer needs the external `cc -c`
# assembler step. Goldens are functional (assemble + link with system
# linker on Linux) plus structural byte pins.
# On non-Linux-riscv64 hosts the suite is skipped: the integrated ELF
# writer is host-format scoped.
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-rv64-asm-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

HOST="$(uname -s)-$(uname -m)"
if [ "$HOST" != "Linux-riscv64" ]; then
  echo "rv64_asm: skipped (host is $HOST, integrated ELF writer is Linux-riscv64 only)"
  exit 0
fi

if [ ! -x "$TIQ" ]; then
  echo "rv64_asm: missing $TIQ" >&2
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

# `fence` is not part of the QBE rv64 subset and must fail closed.
printf '\tfence\n' > "$TMP_DIR/fence.s"
rc=0; $TIQ emit-obj "$TMP_DIR/fence.s" -o "$TMP_DIR/fence.o" 2>"$TMP_DIR/fence.err" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "unsupported: expected exit 1, got $rc" >&2
  exit 1
fi
if ! grep -q "fence.s:1: error" "$TMP_DIR/fence.err"; then
  echo "unsupported: expected located diagnostic 'fence.s:1: error'" >&2
  cat "$TMP_DIR/fence.err" >&2
  exit 1
fi

# --- Test 4: li immediate + external call (exit code) ----------------------

printf '.text\n.globl main\nmain:\n\tli a0, 42\n\tcall exit\n' > "$TMP_DIR/exit42.s"
expect_exit exit42 "$TMP_DIR/exit42.s" 42

# --- Test 5: loop, beqz, unconditional jump, add ----------------------------

# sum_{i=5..1} i = 15
cat > "$TMP_DIR/loop.s" <<'EOF'
.text
.globl main
main:
	li	s0, 0
	li	s1, 5
.L1:
	beqz	s1, .L2
	add	s0, s0, s1
	addi	s1, s1, -1
	j	.L1
.L2:
	mv	a0, s0
	call	exit
EOF
expect_exit loop "$TMP_DIR/loop.s" 15

# --- Test 6: internal function call via call + ret --------------------------

cat > "$TMP_DIR/call.s" <<'EOF'
.text
.globl main
main:
	call	_helper
	call	exit
_helper:
	li	a0, 7
	ret
EOF
expect_exit call "$TMP_DIR/call.s" 7

# --- Test 7: ELF structural bytes -------------------------------------------

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
# e_machine = EM_RISCV (243 = 0x00F3) at offset 18 (little endian: F3 00)
emachine=$(od -An -tx1 -j18 -N2 "$TMP_DIR/struct.o" | tr -d ' \n')
if [ "$emachine" != "f300" ]; then
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
  if ! grep -q "RISC-V" "$TMP_DIR/readelf.out"; then
    echo "readelf: expected RISC-V machine" >&2
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

echo "RISC-V ELF object writer tests passed"