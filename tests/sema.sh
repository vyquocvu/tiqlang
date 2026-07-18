#!/bin/sh
set -e

TMP_DIR="${TMPDIR:-/tmp}/tiq-sema-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
    echo "$1" >&2
    # Use return and structure script differently to avoid exit, or let the shell handle it.
    # We can just exit from subshell or use normal exit if not executed through run_in_bash_session directly
}

assert_sema_err() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output_file="$TMP_DIR/$name.out"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"

  if ./build/tiq check-sema "$input" > "$output_file" 2>&1; then
    echo "expected $name sema to output errors but it succeeded" >&2
    return 1
  fi

  printf '%s\n' "$expected" > "$expected_file"
  if ! grep -qF "$expected" "$output_file"; then
    echo "sema mismatch for $name" >&2
    echo "expected to find: $expected" >&2
    echo "actual output:" >&2
    cat "$output_file" >&2
    return 1
  fi
}

assert_sema_err "duplicate_declaration" 'x = 1
x = 2' "duplicate declaration"

assert_sema_err "undefined_name" 'x = y' "undefined name"

echo "sema: ok"
