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


assert_semantic "immutable_assignment" 'x = 1
x += 1
' "$TMP_DIR/immutable_assignment.tiq:2: error[E11]: cannot assign to immutable binding"

assert_semantic "function_arity_mismatch" 'f a -> a
x = f(1, 2)
' "$TMP_DIR/function_arity_mismatch.tiq:2: error[E12]: arity mismatch"

# M12.4: type annotations are now supported.
# Test return type mismatch detection.
assert_semantic "func_return_type_mismatch" 'f a:i32 -> str -> a
' "$TMP_DIR/func_return_type_mismatch.tiq:1: error[E09]: return type mismatch: expected str, found i32"

# Test unknown type name rejection.
assert_semantic "func_unknown_type" 'f a:unknown -> a
' "$TMP_DIR/func_unknown_type.tiq:1: error[E09]: unknown type 'unknown'"

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

# M12.4: Test that annotated parameters work correctly.
assert_semantic_ast "typed_func_annot" 'add a:i32 b:i32 -> a + b
' 'FUNCTION add <TYPE_I32>
  PARAM a
  PARAM b
  BINARY PLUS <TYPE_I32>
    IDENT a <TYPE_I32>
    IDENT b <TYPE_I32>'

# M9.1: reference parameters auto-deref to the referent type in the body.
assert_semantic_ast "typed_borrow_param" 'show v:&i64 -> v + 0
' 'FUNCTION show <TYPE_INT>
  PARAM v
  BINARY PLUS <TYPE_INT>
    IDENT v <TYPE_INT>
    INT 0 <TYPE_INT>'

# M12.3: i8(1) is now a valid explicit conversion (was fail-closed stub E10).
assert_semantic_ast "typed_conversion_i8" 'x = i8(1)
' 'BINDING x <TYPE_I8>
  CALL <TYPE_I8>
    IDENT i8
    INT 1 <TYPE_INT>'

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

# M12.7.1: stream generators support at most 2 seeds (v0.1 window size)
assert_semantic "stream_too_many_seeds" 'fib = [0, 1, 2, ... a + b + c]
' "$TMP_DIR/stream_too_many_seeds.tiq:1: error[E07]: stream generators support at most 2 seeds"

# M12.7.1: bounded stream generators (while/until) are not yet implemented
assert_semantic "stream_bounded_while" 'fib = [0, 1, ... a + b while x < 100]
' "$TMP_DIR/stream_bounded_while.tiq:1: error[E07]: bounded stream generators are not yet supported"
assert_semantic "stream_bounded_until" 'fib = [0, 1, ... a + b until x > 100]
' "$TMP_DIR/stream_bounded_until.tiq:1: error[E07]: bounded stream generators are not yet supported"

# M12.7.2: match expressions must have a wildcard arm
assert_semantic "match_no_wildcard" 'x = 10
res = match x { 10 => 100 }
' "$TMP_DIR/match_no_wildcard.tiq:2: error[E07]: match must have a wildcard arm ('_ => ...')"

# M12.7.2: range expressions are only valid inside loop or slice contexts
assert_semantic "range_outside_context" 'x = 0..10
' "$TMP_DIR/range_outside_context.tiq:1: error[E07]: range expressions 'a..b' are only valid inside loop or slice contexts"

# M12.7.1: block expressions are only supported in function bodies
# Outside function bodies, they produce a fatal emitter error
assert_semantic "block_outside_function" 'x = { 1 }
' "$TMP_DIR/block_outside_function.tiq:1: error[E07]: block expression not supported outside function body"

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
y = match x { 1 => 2, 2 => "s", _ => 0 }
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

# M9.1: borrows are only valid in call argument position for &/&mut
# parameters (LANGUAGE_SPEC §16.3); anywhere else they fail closed.
assert_semantic "borrow_unsupported" 'x <- 42
b = &x
' "$TMP_DIR/borrow_unsupported.tiq:2: error[E07]: borrow is only valid as an argument to a reference parameter"

assert_semantic "mut_borrow_unsupported" 'x <- 42
b = &mut x
' "$TMP_DIR/mut_borrow_unsupported.tiq:2: error[E07]: borrow is only valid as an argument to a reference parameter"

# M9.1: &mut requires a mutable binding.
assert_semantic "borrow_mut_of_immutable" 'bump r:&mut i64 -> { r <- r + 1 }
x = 1
bump(&mut x)
' "$TMP_DIR/borrow_mut_of_immutable.tiq:3: error[E23]: cannot borrow immutable binding 'x' as mutable"

# M9.1: a reference parameter requires a borrow argument.
assert_semantic "borrow_arg_missing" 'bump r:&mut i64 -> { r <- r + 1 }
x <- 1
bump(x)
' "$TMP_DIR/borrow_arg_missing.tiq:3: error[E23]: argument 1 must be borrowed with &mut"

# M9.1: borrow spelling must match the parameter exactly.
assert_semantic "borrow_kind_mismatch" 'show v:&i64 -> print(v)
x <- 1
show(&mut x)
' "$TMP_DIR/borrow_kind_mismatch.tiq:3: error[E23]: argument 1 must be borrowed with &"

# M9.1: value parameters do not accept borrows.
assert_semantic "borrow_to_value_param" 'f a:i64 -> a
x <- 1
y = f(&x)
' "$TMP_DIR/borrow_to_value_param.tiq:3: error[E23]: argument 1 cannot be a borrow: parameter is by value"

# M9.1: aliasing checks within one call.
assert_semantic "borrow_alias_two_muts" 'f a:&mut i64 b:&mut i64 -> { a <- a + b }
x <- 1
f(&mut x, &mut x)
' "$TMP_DIR/borrow_alias_two_muts.tiq:3: error[E23]: cannot borrow 'x' as mutable more than once in a call"

assert_semantic "borrow_alias_mut_and_shared" 'f a:&mut i64 b:&i64 -> { a <- a + b }
x <- 1
f(&mut x, &x)
' "$TMP_DIR/borrow_alias_mut_and_shared.tiq:3: error[E23]: cannot borrow 'x' as both mutable and shared in a call"

# M9.1: assignment through a shared borrow is rejected.
assert_semantic "assign_through_shared_borrow" 'f r:&i64 -> { r <- 1 }
x <- 1
f(&x)
' "$TMP_DIR/assign_through_shared_borrow.tiq:1: error[E23]: cannot assign through shared borrow 'r'"

# M9.1: reference parameters cannot be re-borrowed.
assert_semantic "borrow_reborrow" 'g r:&i64 -> print(r)
f r:&i64 -> g(&r)
' "$TMP_DIR/borrow_reborrow.tiq:2: error[E23]: cannot re-borrow reference parameter 'r'"

# M9.1: borrowing a moved binding is a use-after-move error.
assert_semantic "borrow_after_move" 'show v:&i64 -> print(v)
x <- 1
y <- move x
show(&x)
' "$TMP_DIR/borrow_after_move.tiq:4: error[E18]: use of moved value 'x'"

# M12.3: explicit numeric type conversions.
# Numeric -> numeric: allowed.
assert_semantic_ast "typed_conversion_int_to_f64" 'x = f64(42)
' 'BINDING x <TYPE_FLOAT>
  CALL <TYPE_FLOAT>
    IDENT f64
    INT 42 <TYPE_INT>'

assert_semantic_ast "typed_conversion_f64_to_int" 'x = i64(3.14)
' 'BINDING x <TYPE_INT>
  CALL <TYPE_INT>
    IDENT i64
    LITERAL <TYPE_FLOAT>'

assert_semantic_ast "typed_conversion_i32" 'x = i32(100)
' 'BINDING x <TYPE_I32>
  CALL <TYPE_I32>
    IDENT i32
    INT 100 <TYPE_INT>'

# str -> numeric: rejected E10.
assert_semantic "conversion_str_to_int" 'x = i32("hello")
' "$TMP_DIR/conversion_str_to_int.tiq:1: error[E10]: cannot convert str to i32"

# bool -> numeric: rejected E10.
assert_semantic "conversion_bool_to_int" 'x = i32(true)
' "$TMP_DIR/conversion_bool_to_int.tiq:1: error[E10]: cannot convert bool to i32"

# numeric -> bool: rejected E10.
assert_semantic "conversion_int_to_bool" 'x = bool(42)
' "$TMP_DIR/conversion_int_to_bool.tiq:1: error[E10]: cannot convert int to bool"

# Arity 0: rejected E12.
assert_semantic "conversion_arity_zero" 'x = i32()
' "$TMP_DIR/conversion_arity_zero.tiq:1: error[E12]: type conversion requires exactly 1 argument"

# Arity 2: rejected E12.
assert_semantic "conversion_arity_two" 'x = i32(1, 2)
' "$TMP_DIR/conversion_arity_two.tiq:1: error[E12]: type conversion requires exactly 1 argument"

# M12.3 exit criteria: width mixing without explicit conversion is rejected.
assert_semantic "width_mixing_i32_i64" 'a = i32(1)
x = a + 2
' "$TMP_DIR/width_mixing_i32_i64.tiq:2: error[E09]: type mismatch: expected i32, found int"

assert_semantic "width_mixing_u8_f64" 'a = u8(1)
x = a + 1.0
' "$TMP_DIR/width_mixing_u8_f64.tiq:2: error[E09]: type mismatch: expected u8, found float"

# Sized types are printable (typed AST).
assert_semantic_ast "typed_print_i32" 'x = i32(42)
print(x)
' 'BINDING x <TYPE_I32>
  CALL <TYPE_I32>
    IDENT i32
    INT 42 <TYPE_INT>
CALL <TYPE_INT>
  IDENT print
  IDENT x <TYPE_I32>'

# M12.6: Struct definition and record literal tests.
assert_semantic "struct_unknown_field_type" 'struct Point {
  x: unknown
}
' "$TMP_DIR/struct_unknown_field_type.tiq:1: error[E09]: unknown field type 'unknown'"

assert_semantic "struct_duplicate" 'struct Point {
  x: i64
}
struct Point {
  y: i64
}
' "$TMP_DIR/struct_duplicate.tiq:4: error[E09]: duplicate struct definition 'Point'"

assert_semantic "record_lit_unknown_struct" 'p = Unknown { x: 1 }
' "$TMP_DIR/record_lit_unknown_struct.tiq:1: error[E09]: unknown struct 'Unknown'"

assert_semantic "record_lit_unknown_field" 'struct Point {
  x: i64
}
p = Point { y: 1 }
' "$TMP_DIR/record_lit_unknown_field.tiq:4: error[E09]: unknown field 'y'"

assert_semantic "record_lit_field_count" 'struct Point {
  x: i64,
  y: i64
}
p = Point { x: 1 }
' "$TMP_DIR/record_lit_field_count.tiq:5: error[E09]: record literal has 1 fields, struct has 2"

assert_semantic "field_access_non_struct" 'x = 1
y = x.foo
' "$TMP_DIR/field_access_non_struct.tiq:2: error[E09]: field access on non-struct type"

assert_semantic "field_access_unknown_field" 'struct Point {
  x: i64
}
p = Point { x: 1 }
y = p.foo
' "$TMP_DIR/field_access_unknown_field.tiq:5: error[E09]: unknown field 'foo'"

# M8: Option/Result fallback operator tests.
assert_semantic "fallback_non_option" 'x = 1
y = x ?? 0
' "$TMP_DIR/fallback_non_option.tiq:2: error[E09]: fallback operator requires Option or Result on left side"

assert_semantic "some_wrong_arity" 'x = some(1, 2)
' "$TMP_DIR/some_wrong_arity.tiq:1: error[E12]: some expects exactly 1 argument"

assert_semantic "ok_wrong_arity" 'x = ok(1, 2)
' "$TMP_DIR/ok_wrong_arity.tiq:1: error[E12]: ok expects exactly 1 argument"

assert_semantic "err_wrong_arity" 'x = err()
' "$TMP_DIR/err_wrong_arity.tiq:1: error[E12]: err expects exactly 1 argument"

assert_semantic "propagate_non_option" 'x = 1
y = x?
' "$TMP_DIR/propagate_non_option.tiq:2: error[E09]: propagation operator requires Option or Result operand"

echo "semantic: ok"
