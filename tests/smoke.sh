#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-smoke-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

./build/tiq --version
./build/tiq emit-c examples/hello.tiq > "$TMP_DIR/hello.c"
./build/tiq build examples/hello.tiq -o "$TMP_DIR/hello"

OUTPUT="$($TMP_DIR/hello)"
[ "$OUTPUT" = "Hello from Tiq" ]

SPECIAL_OUTPUT="$TMP_DIR/hello path;quote\"name"
./build/tiq build examples/hello.tiq -o "$SPECIAL_OUTPUT"
SPECIAL_RESULT="$("$SPECIAL_OUTPUT")"
[ "$SPECIAL_RESULT" = "Hello from Tiq" ]

SPECIAL_TMPDIR="$TMP_DIR/tmp path;quote\"dir"
mkdir -p "$SPECIAL_TMPDIR"
TMPDIR="$SPECIAL_TMPDIR" ./build/tiq build examples/hello.tiq -o "$TMP_DIR/tmpdir-hello"
TMPDIR_RESULT="$($TMP_DIR/tmpdir-hello)"
[ "$TMPDIR_RESULT" = "Hello from Tiq" ]
if find "$SPECIAL_TMPDIR" -mindepth 1 -print | grep . >/dev/null; then
  echo "temporary C file was not removed" >&2
  exit 1
fi

if env CC="$TMP_DIR/missing-cc" ./build/tiq build examples/hello.tiq -o "$TMP_DIR/no-compiler" 2>"$TMP_DIR/missing-cc.err"; then
  echo "expected missing host C compiler to fail" >&2
  exit 1
fi
if [ -e "$TMP_DIR/no-compiler" ]; then
  echo "missing host C compiler unexpectedly produced an executable" >&2
  exit 1
fi
grep 'cannot execute host C compiler' "$TMP_DIR/missing-cc.err" >/dev/null

printf '%s\n' '%invalid' > "$TMP_DIR/invalid.tiq"
if ./build/tiq build "$TMP_DIR/invalid.tiq" -o "$TMP_DIR/invalid" 2>/dev/null; then
  echo "expected invalid program to fail" >&2
  exit 1
fi

printf 'x := 0\n[0..5 | x += 1]\n!x\n' > "$TMP_DIR/bracket_loop_count.tiq"
./build/tiq build "$TMP_DIR/bracket_loop_count.tiq" -o "$TMP_DIR/bracket_loop_count" 2>"$TMP_DIR/bracket_loop_count.err"
LOOP_OUTPUT="$($TMP_DIR/bracket_loop_count)"
[ "$LOOP_OUTPUT" = "5" ] || { echo "bracket_loop_count expected 5, got: $LOOP_OUTPUT"; exit 1; }

printf 'total := 0\n[0..5 | total += i]\n!total\n' > "$TMP_DIR/bracket_sum.tiq"
./build/tiq build "$TMP_DIR/bracket_sum.tiq" -o "$TMP_DIR/bracket_sum" 2>"$TMP_DIR/bracket_sum.err"
FOR_OUTPUT="$($TMP_DIR/bracket_sum)"
[ "$FOR_OUTPUT" = "10" ] || { echo "bracket_sum expected 10, got: $FOR_OUTPUT"; exit 1; }

printf '[0..3 | !i]\n' > "$TMP_DIR/bracket_print.tiq"
./build/tiq build "$TMP_DIR/bracket_print.tiq" -o "$TMP_DIR/bracket_print" 2>"$TMP_DIR/bracket_print.err"
FOR_PRINT_OUTPUT="$($TMP_DIR/bracket_print)"
EXPECTED="0
1
2"
[ "$FOR_PRINT_OUTPUT" = "$EXPECTED" ] || { echo "bracket_print expected '$EXPECTED', got '$FOR_PRINT_OUTPUT'"; exit 1; }

printf '[0..3 | !i, break]\n' > "$TMP_DIR/bracket_break.tiq"
./build/tiq build "$TMP_DIR/bracket_break.tiq" -o "$TMP_DIR/bracket_break" 2>"$TMP_DIR/bracket_break.err"
BREAK_OUTPUT="$($TMP_DIR/bracket_break)"
[ "$BREAK_OUTPUT" = "0" ] || { echo "bracket_break expected 0, got: $BREAK_OUTPUT"; exit 1; }

printf 'x := 0\n[0..3 | x += 1, skip, x += 100]\n!x\n' > "$TMP_DIR/bracket_skip.tiq"
./build/tiq build "$TMP_DIR/bracket_skip.tiq" -o "$TMP_DIR/bracket_skip" 2>"$TMP_DIR/bracket_skip.err"
SKIP_OUTPUT="$($TMP_DIR/bracket_skip)"
[ "$SKIP_OUTPUT" = "3" ] || { echo "bracket_skip expected 3, got: $SKIP_OUTPUT"; exit 1; }

printf 'x := 0\n[x < 5 | x += 1]\n!x\n' > "$TMP_DIR/while_loop.tiq"
./build/tiq build "$TMP_DIR/while_loop.tiq" -o "$TMP_DIR/while_loop" 2>"$TMP_DIR/while_loop.err"
WHILE_OUTPUT="$($TMP_DIR/while_loop)"
[ "$WHILE_OUTPUT" = "5" ] || { echo "while_loop expected 5, got: $WHILE_OUTPUT"; exit 1; }

printf 'xs := [1, 2, 3]\n!xs[0]\n!xs[1]\n!xs[2]\n' > "$TMP_DIR/array_literal.tiq"
./build/tiq build "$TMP_DIR/array_literal.tiq" -o "$TMP_DIR/array_literal" 2>"$TMP_DIR/array_literal.err"
ARRAY_OUTPUT="$($TMP_DIR/array_literal)"
ARRAY_EXPECTED="1
2
3"
[ "$ARRAY_OUTPUT" = "$ARRAY_EXPECTED" ] || { echo "array_literal expected '$ARRAY_EXPECTED', got '$ARRAY_OUTPUT'"; exit 1; }

printf 'xs := [1, 2, 3]\nxs[0] <- 99\n!xs[0]\n!xs[1]\n!xs[2]\n' > "$TMP_DIR/array_assign.tiq"
./build/tiq build "$TMP_DIR/array_assign.tiq" -o "$TMP_DIR/array_assign" 2>"$TMP_DIR/array_assign.err"
ASSIGN_OUTPUT="$($TMP_DIR/array_assign)"
ASSIGN_EXPECTED="99
2
3"
[ "$ASSIGN_OUTPUT" = "$ASSIGN_EXPECTED" ] || { echo "array_assign expected '$ASSIGN_EXPECTED', got '$ASSIGN_OUTPUT'"; exit 1; }

echo "smoke: ok"
