#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-parser-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

assert_parser() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output_file="$TMP_DIR/$name.out"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"
  if ! ./build/tiq dump-ast "$input" > "$output_file"; then
    echo "expected $name parser to pass" >&2
    exit 1
  fi
  printf '%s\n' "$expected" > "$expected_file"
  if ! cmp -s "$expected_file" "$output_file"; then
    echo "parser mismatch for $name" >&2
    echo "expected:" >&2
    cat "$expected_file" >&2
    echo "actual:" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

assert_parser "math" 'x = 1 + 2 * 3 - 4 / 2' 'BINDING x
  BINARY MINUS
    BINARY PLUS
      INT 1
      BINARY STAR
        INT 2
        INT 3
    BINARY SLASH
      INT 4
      INT 2'

assert_parser "logic_and_compare" 'y = a > b && c <= d || !e' 'BINDING y
  BINARY OR_OR
    BINARY AND_AND
      BINARY GT
        IDENT a
        IDENT b
      BINARY LTE
        IDENT c
        IDENT d
    UNARY BANG
      IDENT e'

assert_parser "conditional" 'z = a ? b : c ? d : e' 'BINDING z
  CONDITIONAL
    IDENT a
    IDENT b
    CONDITIONAL
      IDENT c
      IDENT d
      IDENT e'

assert_parser "function_and_block" 'f a b -> {
  a + b
}' 'FUNCTION f
  PARAM a
  PARAM b
  BLOCK
    BINARY PLUS
      IDENT a
      IDENT b'

assert_parser "call_and_assign" 'val <- f(1, 2)
val += 3' 'MUT_BINDING val
  CALL
    IDENT f
    INT 1
    INT 2
ASSIGN val PLUS_EQ
  INT 3'

assert_parser "bracket_loop" 'x <- 0
[0..3 | x += i]' 'MUT_BINDING x
  INT 0
BRACKET_LOOP
  BINARY DOT_DOT
    INT 0
    INT 3
  ASSIGN x PLUS_EQ
    IDENT i'

assert_parser "bracket_loop_print" '[0..3 | i]' 'BRACKET_LOOP
  BINARY DOT_DOT
    INT 0
    INT 3
  IDENT i'

assert_parser "bracket_loop_break" '[0..3 | i, break]' 'BRACKET_LOOP
  BINARY DOT_DOT
    INT 0
    INT 3
  IDENT i
  BREAK'

assert_parser "bracket_loop_skip" '[0..3 | i, skip]' 'BRACKET_LOOP
  BINARY DOT_DOT
    INT 0
    INT 3
  IDENT i
  SKIP'

assert_parser "bracket_expr" 'x = [1 + 2]' 'BINDING x
  BRACKET_EXPR
    BINARY PLUS
      INT 1
      INT 2'

assert_parser "slice_full" 'sub = xs[1..3]' 'BINDING sub
  SLICE
    IDENT xs
    INT 1
    INT 3'

assert_parser "slice_omitted_end" 'tail = xs[1..]' 'BINDING tail
  SLICE
    IDENT xs
    INT 1
    OMITTED'

assert_parser "slice_omitted_start" 'head = xs[..2]' 'BINDING head
  SLICE
    IDENT xs
    OMITTED
    INT 2'

assert_parser "slice_omitted_both" 'full = xs[..]' 'BINDING full
  SLICE
    IDENT xs
    OMITTED
    OMITTED'

assert_parser "stream_gen" 'fib = [0, 1, ... a + b]' 'BINDING fib
  STREAM_GEN
    INT 0
    INT 1
    BINARY PLUS
      IDENT a
      IDENT b'

assert_parser "stream_gen_single_seed" 'pow b -> [1, ... x * b]' 'FUNCTION pow
  PARAM b
  STREAM_GEN
    INT 1
    BINARY STAR
      IDENT x
      IDENT b'

assert_parser "move_expr" 'y <- move x' 'MUT_BINDING y
  UNARY MOVE
    IDENT x'

assert_parser "defer_stmt" '{defer 1}' 'BLOCK
  DEFER
    INT 1'

echo "parser: ok"
