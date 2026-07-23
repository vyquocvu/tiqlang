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
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"
  if ./build/tiq build "$input" -o "$output_file" 2>"$stderr_file"; then
    echo "expected $name to fail" >&2
    exit 1
  fi
  if [ -e "$output_file" ]; then
    echo "$name unexpectedly produced an executable" >&2
    exit 1
  fi
  printf '%s\n' "$expected" > "$expected_file"
  if ! cmp -s "$expected_file" "$stderr_file"; then
    echo "diagnostic mismatch for $name" >&2
    echo "expected:" >&2
    cat "$expected_file" >&2
    echo "actual:" >&2
    cat "$stderr_file" >&2
    exit 1
  fi
}

assert_diagnostic "missing_string" '!
' "$TMP_DIR/missing_string.tiq:1: error: expected expression after '!'"
assert_diagnostic "newline_in_string" '!"hello
world"' "$TMP_DIR/newline_in_string.tiq:1: error: newline in string literal"
assert_diagnostic "unterminated_string" '!"hello' "$TMP_DIR/unterminated_string.tiq:1: error: unterminated string literal"
assert_diagnostic "second_line_location" '!"ok"
abc
' "$TMP_DIR/second_line_location.tiq:2: error: undefined symbol 'abc'"

assert_diagnostic "while_no_block" 'x := 0
while x < 3
' "$TMP_DIR/while_no_block.tiq:2: error: expected block after while condition"

assert_diagnostic "for_no_in" 'for i x
' "$TMP_DIR/for_no_in.tiq:1: error: expected 'in' after loop variable"

assert_diagnostic "for_no_range" 'for i in 5
' "$TMP_DIR/for_no_range.tiq:1: error: for-in loop requires a range expression"

echo "diagnostics: ok"
