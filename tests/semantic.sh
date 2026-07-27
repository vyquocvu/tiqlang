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
' "$TMP_DIR/undefined_symbol.tiq:1: error[E08]: undefined symbol 'y'"

assert_semantic "out_of_scope" 'x = 1
y = z
' "$TMP_DIR/out_of_scope.tiq:2: error[E08]: undefined symbol 'z'"

assert_semantic "function_scope" 'f a -> a + b
' "$TMP_DIR/function_scope.tiq:1: error[E08]: undefined symbol 'b'"

assert_semantic "type_mismatch" 'x = 1 + "foo"
' "$TMP_DIR/type_mismatch.tiq:1: error[E09]: type mismatch: expected int, found str"

assert_semantic "local_inference_mismatch" 'x = 1
y = x + "foo"
' "$TMP_DIR/local_inference_mismatch.tiq:2: error[E09]: type mismatch: expected int, found str"

assert_semantic "explicit_conversion_unsupported" 'i8(1)
' "$TMP_DIR/explicit_conversion_unsupported.tiq:1: error[E10]: unsupported conversion"

assert_semantic "immutable_assignment" 'x = 1
x += 1
' "$TMP_DIR/immutable_assignment.tiq:2: error[E11]: cannot assign to immutable binding"

assert_semantic "function_arity_mismatch" 'f a -> a
x = f(1, 2)
' "$TMP_DIR/function_arity_mismatch.tiq:2: error[E12]: arity mismatch"

assert_semantic "int_literal_overflow" 'x = 9223372036854775808
' "$TMP_DIR/int_literal_overflow.tiq:1: error[E20]: integer literal out of range for i64"

assert_semantic "int_literal_overflow_expr" 'y = 1 + 99999999999999999999
' "$TMP_DIR/int_literal_overflow_expr.tiq:1: error[E20]: integer literal out of range for i64"

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

assert_semantic_ast "typed_array_nested" 'x = [1, 2, 3]' 'BINDING x <TYPE_ARRAY[3]:TYPE_INT>
  ARRAY <TYPE_ARRAY[3]:TYPE_INT>
    INT 1 <TYPE_INT>
    INT 2 <TYPE_INT>
    INT 3 <TYPE_INT>'

assert_semantic_ast "typed_slice_nested" 'x = [1, 2, 3]
y = x[1..3]' 'BINDING x <TYPE_ARRAY[3]:TYPE_INT>
  ARRAY <TYPE_ARRAY[3]:TYPE_INT>
    INT 1 <TYPE_INT>
    INT 2 <TYPE_INT>
    INT 3 <TYPE_INT>
BINDING y <TYPE_SLICE:TYPE_INT>
  SLICE <TYPE_SLICE:TYPE_INT>
    IDENT x <TYPE_ARRAY[3]:TYPE_INT>
    INT 1 <TYPE_INT>
    INT 3 <TYPE_INT>'

assert_semantic_ast "typed_bracket_loop" 'x <- 0
[0..3] { x += i }' 'MUT_BINDING x <TYPE_INT>
  INT 0 <TYPE_INT>
BRACKET_LOOP <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  ASSIGN x PLUS_EQ <TYPE_INT>
    IDENT i <TYPE_INT>'

assert_semantic_ast "typed_singleton_array" 'x = [1 + 2]' 'BINDING x <TYPE_ARRAY[1]:TYPE_INT>
  ARRAY <TYPE_ARRAY[1]:TYPE_INT>
    BINARY PLUS <TYPE_INT>
      INT 1 <TYPE_INT>
      INT 2 <TYPE_INT>'

assert_semantic_ast "typed_unary_not" 'x = !false' 'BINDING x <TYPE_BOOL>
  UNARY BANG <TYPE_BOOL>
    LITERAL <TYPE_BOOL>'

assert_semantic "bang_requires_bool" 'x = !5
' "$TMP_DIR/bang_requires_bool.tiq:1: error[E09]: operand of '!' must be bool, found int"

assert_semantic_ast "typed_break_bracket" '[0..3] { break }' 'BRACKET_LOOP <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  BREAK <TYPE_UNKNOWN>'

assert_semantic_ast "typed_skip_bracket" '[0..3] { skip }' 'BRACKET_LOOP <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  SKIP <TYPE_UNKNOWN>'

assert_semantic "break_outside_loop" 'break
' "$TMP_DIR/break_outside_loop.tiq:1: error[E16]: break outside loop"

assert_semantic "skip_outside_loop" 'skip
' "$TMP_DIR/skip_outside_loop.tiq:1: error[E16]: skip outside loop"

assert_semantic "loop_cond_type" 'x <- 0
[1] { x += 1 }
' "$TMP_DIR/loop_cond_type.tiq:2: error[E14]: loop condition must be bool"

assert_semantic "loop_range_type" '["a".."b"] { 0 }
' "$TMP_DIR/loop_range_type.tiq:1: error[E09]: range bounds must be int"

assert_semantic "loop_range_mixed_type" '[0.."b"] { 0 }
' "$TMP_DIR/loop_range_mixed_type.tiq:1: error[E09]: type mismatch: expected int, found str
$TMP_DIR/loop_range_mixed_type.tiq:1: error[E09]: range bounds must be int"

assert_semantic "loop_cond_float" '[1.0] { 0 }
' "$TMP_DIR/loop_cond_float.tiq:1: error[E14]: loop condition must be bool"

assert_semantic_ast "typed_loop_binder" 'x <- 0
[j <- 0..3] { x += j }' 'MUT_BINDING x <TYPE_INT>
  INT 0 <TYPE_INT>
BRACKET_LOOP j <TYPE_UNKNOWN>
  BINARY DOT_DOT <TYPE_INT>
    INT 0 <TYPE_INT>
    INT 3 <TYPE_INT>
  ASSIGN x PLUS_EQ <TYPE_INT>
    IDENT j <TYPE_INT>'

assert_semantic "loop_binder_non_range" 'x <- 0
[j <- x < 3] { x += 1 }
' "$TMP_DIR/loop_binder_non_range.tiq:2: error[E15]: loop binder requires a range domain"

assert_semantic "loop_index_immutable" '[0..3] { i <- 1 }
' "$TMP_DIR/loop_index_immutable.tiq:1: error[E11]: cannot assign to immutable binding"

assert_semantic "loop_binder_immutable" '[j <- 0..3] { j += 1 }
' "$TMP_DIR/loop_binder_immutable.tiq:1: error[E11]: cannot assign to immutable binding"

assert_semantic "loop_binder_hides_default_index" '[j <- 0..3] { print(i) }
' "$TMP_DIR/loop_binder_hides_default_index.tiq:1: error[E08]: undefined symbol 'i'"

assert_semantic "array_mixed_types" 'x = [1, "foo"]
' "$TMP_DIR/array_mixed_types.tiq:1: error[E09]: array elements must have the same type: expected int, found str"

assert_semantic "array_index_non_int" 'x = [1, 2, 3]
y = x["foo"]
' "$TMP_DIR/array_index_non_int.tiq:2: error[E09]: array index must be int"

assert_semantic "array_assign_immutable" 'x = [1, 2, 3]
x[0] <- 99
' "$TMP_DIR/array_assign_immutable.tiq:2: error[E11]: cannot assign to immutable binding"

assert_semantic "array_assign_bad_index" 'x <- [1, 2, 3]
x["bad"] <- 99
' "$TMP_DIR/array_assign_bad_index.tiq:2: error[E09]: array index must be int"

assert_semantic "len_non_array" 'x <- 42
y = len(x)
' "$TMP_DIR/len_non_array.tiq:2: error[E09]: len expects an array argument"

assert_semantic "len_no_args" 'y = len()
' "$TMP_DIR/len_no_args.tiq:1: error[E12]: len expects exactly 1 argument"

# M12.7.1: empty arrays are rejected with cannot-infer-element-type
assert_semantic "empty_array" 'x = []
' "$TMP_DIR/empty_array.tiq:1: error[E21]: cannot infer element type for empty array"

assert_semantic_ast "typed_print_call" 'x = print(42)' 'BINDING x <TYPE_INT>
  CALL <TYPE_INT>
    IDENT print
    INT 42 <TYPE_INT>'

assert_semantic "print_no_args" 'print()
' "$TMP_DIR/print_no_args.tiq:1: error[E12]: print expects exactly 1 argument"

assert_semantic "print_two_args" 'print(1, 2)
' "$TMP_DIR/print_two_args.tiq:1: error[E12]: print expects exactly 1 argument"

assert_semantic "print_unprintable" 'xs = [1, 2, 3]
print(xs)
' "$TMP_DIR/print_unprintable.tiq:2: error[E09]: print cannot print [3]int"

assert_semantic "slice_non_int" 'x = [1, 2, 3]
y = x["bad"..2]
' "$TMP_DIR/slice_non_int.tiq:2: error[E09]: slice index must be int"

assert_semantic "slice_non_array" 'x = 42
y = x[1..2]
' "$TMP_DIR/slice_non_array.tiq:2: error[E09]: cannot slice non-array"

assert_semantic "stream_range_slice" 'fib = [0, 1, ... a + b]
x = fib[0..5]
' "$TMP_DIR/stream_range_slice.tiq:2: error[E09]: cannot range-slice a stream generator"

assert_semantic "stream_index_non_int" 'fib = [0, 1, ... a + b]
x = fib["bad"]
' "$TMP_DIR/stream_index_non_int.tiq:2: error[E09]: stream index must be int"

# Stream generator context binds a/b (two seeds), x (one seed), and i only;
# the undocumented 's' name is not part of the language (DOC_REVIEW E).
assert_semantic "stream_state_undefined" 'fib = [0, 1, ... a + s]
' "$TMP_DIR/stream_state_undefined.tiq:1: error[E08]: undefined symbol 's'"

assert_semantic "move_immutable" 'x = [1, 2, 3]
y <- move x
' "$TMP_DIR/move_immutable.tiq:2: error[E17]: cannot move an immutable binding"

assert_semantic "use_after_move" 'x <- [1, 2, 3]
y <- move x
z = x
' "$TMP_DIR/use_after_move.tiq:3: error[E18]: use of moved value 'x'"

assert_semantic "double_move" 'x <- [1, 2, 3]
y <- move x
z <- move x
' "$TMP_DIR/double_move.tiq:3: error[E18]: use of moved value 'x'"

assert_semantic "defer_outside_block" 'defer 1
' "$TMP_DIR/defer_outside_block.tiq:1: error[E19]: defer is not allowed outside a block"

assert_semantic "fs_read_non_str" 'fs_read(123)
' "$TMP_DIR/fs_read_non_str.tiq:1: error[E09]: fs_read argument: expected str, found int"

assert_semantic "fs_read_no_args" 'fs_read()
' "$TMP_DIR/fs_read_no_args.tiq:1: error[E12]: fs_read expects exactly 1 argument"

assert_semantic "fs_write_bad_args" 'fs_write("path")
' "$TMP_DIR/fs_write_bad_args.tiq:1: error[E12]: fs_write expects exactly 2 arguments"

assert_semantic "fs_write_type_mismatch" 'fs_write(123, "data")
' "$TMP_DIR/fs_write_type_mismatch.tiq:1: error[E09]: fs_write argument: expected str, found int"

assert_semantic "proc_exec_bad_type" 'proc_exec(123)
' "$TMP_DIR/proc_exec_bad_type.tiq:1: error[E09]: proc_exec argument: expected str, found int"

assert_semantic "proc_exit_bad_type" 'proc_exit("bad")
' "$TMP_DIR/proc_exit_bad_type.tiq:1: error[E09]: proc_exit argument: expected int, found str"

assert_semantic "json_parse_int_bad_type" 'json_parse_int(123)
' "$TMP_DIR/json_parse_int_bad_type.tiq:1: error[E09]: json_parse_int argument: expected str, found int"

assert_semantic "json_encode_str_bad_type" 'json_encode_str(123)
' "$TMP_DIR/json_encode_str_bad_type.tiq:1: error[E09]: json_encode_str argument: expected str, found int"

assert_semantic "net_fetch_bad_type" 'net_fetch(123)
' "$TMP_DIR/net_fetch_bad_type.tiq:1: error[E09]: net_fetch argument: expected str, found int"

# unify() (plan 3.1): conditional branches and every match arm are checked.
assert_semantic "conditional_branch_mismatch" 'b = true
x = b ? 2 : "s"
' "$TMP_DIR/conditional_branch_mismatch.tiq:2: error[E09]: type mismatch: expected int, found str"

assert_semantic "match_arm_mismatch" 'x = 1
y = match x { 1 => 2, 2 => "s" }
' "$TMP_DIR/match_arm_mismatch.tiq:2: error[E09]: match arms must have the same type: expected int, found str"

assert_semantic "function_redefinition_mismatch" 'f a -> 1
f a -> "s"
' "$TMP_DIR/function_redefinition_mismatch.tiq:2: error[E09]: type mismatch: expected int, found str"

# M7 runtime does not exist yet: spawn/chan must fail closed (plan 1.2).
assert_semantic "spawn_unsupported" 'sp = spawn 10
' "$TMP_DIR/spawn_unsupported.tiq:1: error[E07]: spawn is not supported yet"

assert_semantic "chan_unsupported" 'ch = chan int
' "$TMP_DIR/chan_unsupported.tiq:1: error[E07]: chan is not supported yet"

assert_semantic "spawn_unsupported_line" 'x = 1
sp = spawn x
' "$TMP_DIR/spawn_unsupported_line.tiq:2: error[E07]: spawn is not supported yet"

# M9 borrow checking does not exist yet: unary borrows must fail closed
# instead of silently emitting a value copy (DOC_REVIEW D).
assert_semantic "borrow_unsupported" 'x <- 42
b = &x
' "$TMP_DIR/borrow_unsupported.tiq:2: error[E07]: borrow is not supported yet"

assert_semantic "mut_borrow_unsupported" 'x <- 42
b = &mut x
' "$TMP_DIR/mut_borrow_unsupported.tiq:2: error[E07]: borrow is not supported yet"

echo "semantic: ok"
