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
assert_diagnostic "bracket_loop_no_pipe" '[0..10' "expected '|' in bracket loop"
assert_diagnostic "bracket_loop_no_rbracket" '[0..10 | i' "expected ']' after bracket loop body"

echo "diagnostics: ok"
