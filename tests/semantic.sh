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

assert_semantic "function_scope" 'f a -> a + b
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

assert_semantic "function_arity_mismatch" 'f a -> a
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

assert_semantic_ast "typed_bracket_loop" 'x <- 0
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

assert_semantic "loop_cond_type" 'x <- 0
[1 | x += 1]
' "$TMP_DIR/loop_cond_type.tiq:2: error: loop condition must be bool"

assert_semantic "loop_range_type" '["a".."b" | !"nope"]
' "$TMP_DIR/loop_range_type.tiq:1: error: range bounds must be int"

assert_semantic "loop_range_mixed_type" '[0.."b" | !"nope"]
' "$TMP_DIR/loop_range_mixed_type.tiq:1: error: type mismatch
$TMP_DIR/loop_range_mixed_type.tiq:1: error: range bounds must be int"

assert_semantic "loop_cond_float" '[1.0 | !"nope"]
' "$TMP_DIR/loop_cond_float.tiq:1: error: loop condition must be bool"

assert_semantic "array_mixed_types" 'x = [1, "foo"]
' "$TMP_DIR/array_mixed_types.tiq:1: error: array elements must have the same type"

assert_semantic "array_index_non_int" 'x = [1, 2, 3]
y = x["foo"]
' "$TMP_DIR/array_index_non_int.tiq:2: error: array index must be int"

assert_semantic "array_assign_immutable" 'x = [1, 2, 3]
x[0] <- 99
' "$TMP_DIR/array_assign_immutable.tiq:2: error: cannot assign to immutable binding"

assert_semantic "array_assign_bad_index" 'x <- [1, 2, 3]
x["bad"] <- 99
' "$TMP_DIR/array_assign_bad_index.tiq:2: error: array index must be int"

assert_semantic "array_print" 'x <- [1, 2, 3]
!x
' "$TMP_DIR/array_print.tiq:2: error: cannot print array directly"

assert_semantic "len_non_array" 'x <- 42
!len(x)
' "$TMP_DIR/len_non_array.tiq:2: error: len expects an array argument"

assert_semantic "len_no_args" '!len()
' "$TMP_DIR/len_no_args.tiq:1: error: len expects exactly 1 argument"

assert_semantic "slice_non_int" 'x = [1, 2, 3]
y = x["bad"..2]
' "$TMP_DIR/slice_non_int.tiq:2: error: slice index must be int"

assert_semantic "slice_non_array" 'x = 42
y = x[1..2]
' "$TMP_DIR/slice_non_array.tiq:2: error: cannot slice non-array"

assert_semantic "stream_print" 'fib = [0, 1, ... a + b]
!fib
' "$TMP_DIR/stream_print.tiq:2: error: cannot print stream generator directly"

assert_semantic "stream_range_slice" 'fib = [0, 1, ... a + b]
x = fib[0..5]
' "$TMP_DIR/stream_range_slice.tiq:2: error: cannot range-slice a stream generator"

assert_semantic "stream_index_non_int" 'fib = [0, 1, ... a + b]
x = fib["bad"]
' "$TMP_DIR/stream_index_non_int.tiq:2: error: stream index must be int"

assert_semantic "move_immutable" 'x = [1, 2, 3]
y <- move x
' "$TMP_DIR/move_immutable.tiq:2: error: cannot move an immutable binding"

assert_semantic "use_after_move" 'x <- [1, 2, 3]
y <- move x
!x
' "$TMP_DIR/use_after_move.tiq:3: error: use of moved value 'x'"

assert_semantic "double_move" 'x <- [1, 2, 3]
y <- move x
z <- move x
' "$TMP_DIR/double_move.tiq:3: error: use of moved value 'x'"

assert_semantic "defer_outside_block" 'defer 1
' "$TMP_DIR/defer_outside_block.tiq:1: error: defer is not allowed outside a block"

echo "semantic: ok"
