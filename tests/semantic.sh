#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-semantic-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

assert_semantic() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  stderr_file="$TMP_DIR/$name.err"
  output_file="$TMP_DIR/$name.out"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"
  if ./build/tiq build "$input" -o "$output_file" 2>"$stderr_file"; then
    echo "expected $name semantic check to fail" >&2
    exit 1
  fi
  if [ -e "$output_file" ]; then
    echo "$name unexpectedly produced an executable" >&2
    exit 1
  fi
  printf '%s\n' "$expected" > "$expected_file"
  if ! cmp -s "$expected_file" "$stderr_file"; then
    echo "semantic mismatch for $name" >&2
    echo "expected:" >&2
    cat "$expected_file" >&2
    echo "actual:" >&2
    cat "$stderr_file" >&2
    exit 1
  fi
}

assert_semantic "undefined_symbol" 'x = y
' "$TMP_DIR/undefined_symbol.tiq:1: error: undefined symbol 'y'"

assert_semantic "out_of_scope" 'x = 1
y = z
' "$TMP_DIR/out_of_scope.tiq:2: error: undefined symbol 'z'"

assert_semantic "function_scope" 'f a = a + b
' "$TMP_DIR/function_scope.tiq:1: error: undefined symbol 'b'"

assert_semantic "type_mismatch" 'x = 1 + "foo"
' "$TMP_DIR/type_mismatch.tiq:1: error: type mismatch"

assert_semantic "local_inference_mismatch" 'x = 1
y = x + "foo"
' "$TMP_DIR/local_inference_mismatch.tiq:2: error: type mismatch"

assert_semantic "explicit_conversion_unsupported" 'i8(1)
' "$TMP_DIR/explicit_conversion_unsupported.tiq:1: error: unsupported conversion"

assert_semantic "immutable_assignment" 'x = 1
x += 1
' "$TMP_DIR/immutable_assignment.tiq:2: error: cannot assign to immutable binding"

assert_semantic "function_arity_mismatch" 'f a = a
x = f(1, 2)
' "$TMP_DIR/function_arity_mismatch.tiq:2: error: arity mismatch"

assert_semantic_ast() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output_file="$TMP_DIR/$name.out"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"
  if ! ./build/tiq dump-typed-ast "$input" > "$output_file"; then
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

assert_semantic_ast "typed_ir_basic" 'x = 1 + 2' 'BINDING x <TYPE_INT>
  BINARY PLUS <TYPE_INT>
    INT 1 <TYPE_INT>
    INT 2 <TYPE_INT>'

assert_semantic_ast "typed_bracket_loop" 'x := 0
[0..3 | x += i]' 'MUT_BINDING x <TYPE_INT>
  INT 0 <TYPE_INT>
BRACKET_LOOP <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  ASSIGN x PLUS_EQ <TYPE_UNKNOWN>
    IDENT i <TYPE_INT>'

assert_semantic_ast "typed_bracket_expr" 'x = [1 + 2]' 'BINDING x <TYPE_INT>
  BRACKET_EXPR <TYPE_INT>
    BINARY PLUS <TYPE_INT>
      INT 1 <TYPE_INT>
      INT 2 <TYPE_INT>'

assert_semantic_ast "typed_break_bracket" '[0..3 | break]' 'BRACKET_LOOP <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  BREAK <TYPE_UNKNOWN>'

assert_semantic_ast "typed_skip_bracket" '[0..3 | skip]' 'BRACKET_LOOP <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  SKIP <TYPE_UNKNOWN>'

assert_semantic "break_outside_loop" 'break
' "$TMP_DIR/break_outside_loop.tiq:1: error: break outside loop"

assert_semantic "skip_outside_loop" 'skip
' "$TMP_DIR/skip_outside_loop.tiq:1: error: skip outside loop"

echo "semantic: ok"
