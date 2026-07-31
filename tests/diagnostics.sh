#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-diagnostics-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

assert_diagnostic() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  stderr_file="$TMP_DIR/$name.err"
  output_file="$TMP_DIR/$name.out"

  printf '%s' "$source" > "$input"
  if ./build/tiq build "$input" -o "$output_file" 2>"$stderr_file"; then
    echo "expected $name to fail" >&2
    exit 1
  fi
  if [ -e "$output_file" ]; then
    echo "$name unexpectedly produced an executable" >&2
    exit 1
  fi
  if ! grep -q "$expected" "$stderr_file"; then
    echo "diagnostic mismatch for $name" >&2
    echo "expected pattern: $expected" >&2
    echo "actual:" >&2
    cat "$stderr_file" >&2
    exit 1
  fi
}

assert_diagnostic "unterminated_string" 'x = \"hello' "unterminated string literal"
# LANGUAGE_SPEC §4: only \\ \" \n \r \t \0 are valid escapes; others fail closed.
assert_diagnostic "bad_string_escape" 'x = "a\q"' "unsupported escape sequence"
assert_diagnostic "bracket_loop_no_rbracket" '[0..10' "expected ']' after loop header"
assert_diagnostic "bracket_loop_no_lbrace" '[0..10] i' "expected '{' to open loop body"
assert_diagnostic "bracket_loop_no_rbrace" '[0..10] { i' "expected '}' after loop body"
# The pre-2026-07-27 separator form must fail closed: '|' inside the
# header is bitwise OR now, so the loop is missing its '{' body.
assert_diagnostic "bracket_loop_old_pipe" '[0..10 | i]' "expected '{' to open loop body"
# Multi-binder headers: every clause after ',' must be 'name <- range',
# and binder names must be distinct.
assert_diagnostic "bracket_loop_binder_missing" '[j <- 0..3, 0..2] { 0 }' "expected loop binder after ','"
assert_diagnostic "bracket_loop_dup_binder" '[j <- 0..3, j <- 0..2] { 0 }' "duplicate loop binder"

# M13.1-P2: malformed enum declarations fail closed at parse time
# (LANGUAGE_SPEC §17.5; no explicit values, top level only).
assert_diagnostic "enum_no_name" 'enum { A }' "expected enum name after 'enum'"
assert_diagnostic "enum_no_lbrace" 'enum Color A' "expected '{' after enum name"
assert_diagnostic "enum_no_rbrace" 'enum Color { A' "expected '}' after enum variants"
assert_diagnostic "enum_explicit_value" 'enum Color { A = 1 }' "expected variant name"
assert_diagnostic "enum_in_block" '{ enum Color { A } }' "expected expression"

# Regression: parser must terminate on invalid input (fuzz findings 0.4).
# Each case previously looped forever because errors neither consumed a
# token nor stopped parsing. 10s alarm converts a hang into a failure.
assert_parser_terminates() {
  name="$1"
  source="$2"
  input="$TMP_DIR/$name.tiq"
  printf '%s' "$source" > "$input"
  set +e
  perl -e 'alarm 10; exec @ARGV' ./build/tiq check "$input" >/dev/null 2>&1
  code=$?
  set -e
  if [ "$code" -eq 0 ]; then
    echo "expected $name to fail" >&2
    exit 1
  fi
  if [ "$code" -gt 1 ]; then
    echo "$name crashed or hung (exit $code)" >&2
    exit 1
  fi
}

assert_parser_terminates "fuzz_bad_index" '[0..10 | !alt[,]]'
assert_parser_terminates "fuzz_bad_domain" '[0(.10 | !i, break]'
assert_parser_terminates "fuzz_bad_args" 'f(1,,,'
assert_parser_terminates "fuzz_bad_match" 'match x { , => 1 }'

echo "diagnostics: ok"
