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

# M15: gated builtins require std/ import.
assert_semantic "json_parse_int_gated" 'json_parse_int(123)
' "$TMP_DIR/json_parse_int_gated.tiq:1: error[E08]: undefined symbol 'json_parse_int' — import \"std/json.tiq\" for JSON operations"

assert_semantic "json_encode_str_gated" 'json_encode_str(123)
' "$TMP_DIR/json_encode_str_gated.tiq:1: error[E08]: undefined symbol 'json_encode_str' — import \"std/json.tiq\" for JSON operations"

assert_semantic "net_fetch_gated" 'net_fetch(123)
' "$TMP_DIR/net_fetch_gated.tiq:1: error[E08]: undefined symbol 'net_fetch' — import \"std/net.tiq\" for networking"

# M10.1: CLI argument builtins (LANGUAGE_SPEC §18.1).
assert_semantic_ast "typed_cli_builtins" 'n = cli_arg_count()
a = cli_arg(0)
' 'BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT cli_arg_count
BINDING a <TYPE_STR>
  CALL <TYPE_STR>
    IDENT cli_arg
    INT 0 <TYPE_INT>'

assert_semantic "cli_arg_count_bad_arity" 'x = cli_arg_count(1)
' "$TMP_DIR/cli_arg_count_bad_arity.tiq:1: error[E12]: cli_arg_count expects exactly 0 arguments"

assert_semantic "cli_arg_no_args" 'x = cli_arg()
' "$TMP_DIR/cli_arg_no_args.tiq:1: error[E12]: cli_arg expects exactly 1 argument"

assert_semantic "cli_arg_bad_type" 'x = cli_arg("0")
' "$TMP_DIR/cli_arg_bad_type.tiq:1: error[E09]: cli_arg argument: expected int, found str"

# M10.2: JSON decoder builtin (LANGUAGE_SPEC §19) — via std/json.tiq import.
assert_semantic_ast "typed_json_get" 'import "std/json.tiq"
v = json_get("{}", "k")
' 'BINDING v <TYPE_STR>
  CALL <TYPE_STR>
    IDENT json_get <TYPE_STR>
    STRING "{}" <TYPE_STR>
    STRING "k" <TYPE_STR>'

assert_semantic "json_get_gated" 'x = json_get("{}")
' "$TMP_DIR/json_get_gated.tiq:1: error[E08]: undefined symbol 'json_get' — import \"std/json.tiq\" for JSON operations"

# M10.3: JSON array builtins (LANGUAGE_SPEC §19) — via std/json.tiq import.
assert_semantic_ast "typed_json_arr" 'import "std/json.tiq"
n = json_arr_len("[1]")
v = json_arr_get("[1]", 0)
' 'BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT json_arr_len <TYPE_INT>
    STRING "[1]" <TYPE_STR>
BINDING v <TYPE_STR>
  CALL <TYPE_STR>
    IDENT json_arr_get <TYPE_STR>
    STRING "[1]" <TYPE_STR>
    INT 0 <TYPE_INT>'

# M15: via the std/ wrapper the call is a plain user-function call, so
# arity uses the generic diagnostic and scalar argument annotations are
# not enforced at the call site (same as any user-defined function).
assert_semantic "json_arr_get_bad_arity" 'import "std/json.tiq"
x = json_arr_get("[1]")
' "$TMP_DIR/json_arr_get_bad_arity.tiq:2: error[E12]: arity mismatch"

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

# M13.1-P1: str_sub / str_eq / eprint / fs_list builtins (LANGUAGE_SPEC §19.5, §19.6).
assert_semantic "str_sub_bad_arity" 'x = str_sub("abc", 0)
' "$TMP_DIR/str_sub_bad_arity.tiq:1: error[E12]: str_sub expects exactly 3 arguments"

assert_semantic "str_sub_bad_str_type" 'x = str_sub(1, 0, 1)
' "$TMP_DIR/str_sub_bad_str_type.tiq:1: error[E09]: str_sub argument: expected str, found int"

assert_semantic "str_sub_bad_index_type" 'x = str_sub("abc", "0", 1)
' "$TMP_DIR/str_sub_bad_index_type.tiq:1: error[E09]: str_sub argument: expected int, found str"

assert_semantic "str_eq_bad_arity" 'x = str_eq("a")
' "$TMP_DIR/str_eq_bad_arity.tiq:1: error[E12]: str_eq expects exactly 2 arguments"

assert_semantic "str_eq_bad_type" 'x = str_eq("a", 1)
' "$TMP_DIR/str_eq_bad_type.tiq:1: error[E09]: str_eq argument: expected str, found int"

assert_semantic "eprint_bad_arity" 'x = eprint("a", "b")
' "$TMP_DIR/eprint_bad_arity.tiq:1: error[E12]: eprint expects exactly 1 argument"

assert_semantic "eprint_bad_type" 'x = eprint(42)
' "$TMP_DIR/eprint_bad_type.tiq:1: error[E09]: eprint argument: expected str, found int"

assert_semantic "fs_list_bad_arity" 'x = fs_list()
' "$TMP_DIR/fs_list_bad_arity.tiq:1: error[E12]: fs_list expects exactly 1 argument"

assert_semantic "fs_list_bad_type" 'x = fs_list(42)
' "$TMP_DIR/fs_list_bad_type.tiq:1: error[E09]: fs_list argument: expected str, found int"

# M14.3: monotonic clock builtin (LANGUAGE_SPEC §19.6).
assert_semantic_ast "typed_clock_ms" 'n = clock_ms()
' 'BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT clock_ms'

assert_semantic "clock_ms_bad_arity" 'x = clock_ms(1)
' "$TMP_DIR/clock_ms_bad_arity.tiq:1: error[E12]: clock_ms expects exactly 0 arguments"

assert_semantic "clock_ms_used_as_str" 'x = clock_ms() + "ms"
' "$TMP_DIR/clock_ms_used_as_str.tiq:1: error[E09]: type mismatch: expected int, found str"

# M13.1-P2: enum declarations (LANGUAGE_SPEC §17.5).
assert_semantic "enum_duplicate" 'enum Color { Red }
enum Color { Blue }
' "$TMP_DIR/enum_duplicate.tiq:2: error[E24]: duplicate enum definition 'Color'"

assert_semantic "enum_struct_collision" 'struct Point { x: i64 }
enum Point { A }
' "$TMP_DIR/enum_struct_collision.tiq:2: error[E24]: enum 'Point' conflicts with struct 'Point'"

assert_semantic "struct_enum_collision" 'enum Color { Red }
struct Color { x: i64 }
' "$TMP_DIR/struct_enum_collision.tiq:2: error[E24]: struct 'Color' conflicts with enum 'Color'"

assert_semantic "enum_duplicate_variant" 'enum Color { Red, Green, Red }
' "$TMP_DIR/enum_duplicate_variant.tiq:1: error[E25]: duplicate variant 'Red' in enum 'Color'"

assert_semantic "enum_unknown_variant" 'enum Color { Red, Green }
x = Color.Purple
' "$TMP_DIR/enum_unknown_variant.tiq:2: error[E26]: unknown variant 'Purple' in enum 'Color'"

assert_semantic "enum_bare_as_value" 'enum Color { Red }
x = Color
' "$TMP_DIR/enum_bare_as_value.tiq:2: error[E09]: enum 'Color' is not a value; use Color.<variant>"

# Variant references are plain ints in the typed IR; the enum wins in
# field-access target position.
assert_semantic_ast "typed_enum_variant" 'enum Color { Red, Green }
x = Color.Green' 'ENUM_DEF Color
  VARIANT Red
  VARIANT Green
BINDING x <TYPE_INT>
  FIELD_ACCESS Green <TYPE_INT>
    IDENT Color <TYPE_INT>'

# M13.1-P3: Vec builtins (LANGUAGE_SPEC §19.7).
assert_semantic "vec_new_bad_arity" 'v = vec_new(1)
' "$TMP_DIR/vec_new_bad_arity.tiq:1: error[E12]: vec_new expects no arguments"

assert_semantic "vec_push_bad_arity" 'v = vec_new()
n = vec_push(v)
' "$TMP_DIR/vec_push_bad_arity.tiq:2: error[E12]: vec_push expects exactly 2 arguments"

assert_semantic "vec_set_bad_arity" 'v = vec_new()
vec_push(v, 1)
vec_set(v, 0)
' "$TMP_DIR/vec_set_bad_arity.tiq:3: error[E12]: vec_set expects exactly 3 arguments"

assert_semantic "vec_len_bad_vec_type" 'x = 1
n = vec_len(x)
' "$TMP_DIR/vec_len_bad_vec_type.tiq:2: error[E09]: vec_len argument: expected vec, found int"

assert_semantic "vec_push_bad_vec_type" 'n = vec_push(1, 2)
' "$TMP_DIR/vec_push_bad_vec_type.tiq:1: error[E09]: vec_push argument: expected vec, found int"

assert_semantic "vec_push_elem_mismatch" 'v = vec_new()
vec_push(v, 1)
vec_push(v, "s")
' "$TMP_DIR/vec_push_elem_mismatch.tiq:3: error[E09]: vec_push element: expected int, found str"

assert_semantic "vec_set_elem_mismatch" 'v = vec_new()
vec_push(v, "a")
vec_set(v, 0, 1)
' "$TMP_DIR/vec_set_elem_mismatch.tiq:3: error[E09]: vec_set element: expected str, found int"

assert_semantic "vec_get_bad_index" 'v = vec_new()
vec_push(v, 1)
x = vec_get(v, "0")
' "$TMP_DIR/vec_get_bad_index.tiq:3: error[E09]: vec_get index: expected int, found str"

assert_semantic "vec_push_bad_elem_kind" 'v = vec_new()
vec_push(v, 1 == 1)
' "$TMP_DIR/vec_push_bad_elem_kind.tiq:2: error[E09]: vec_push element must be int, str, or a struct"

# Fail-closed rule: only vec_push establishes the element type; get/set/pop
# on a vec that never saw a push are compile-time errors (§19.7).
assert_semantic "vec_get_unestablished" 'v = vec_new()
x = vec_get(v, 0)
' "$TMP_DIR/vec_get_unestablished.tiq:2: error[E09]: vec_get on a vec with no established element type (no vec_push yet)"

assert_semantic "vec_set_unestablished" 'v = vec_new()
vec_set(v, 0, 1)
' "$TMP_DIR/vec_set_unestablished.tiq:2: error[E09]: vec_set on a vec with no established element type (no vec_push yet)"

assert_semantic "vec_pop_unestablished" 'v = vec_new()
x = vec_pop(v)
' "$TMP_DIR/vec_pop_unestablished.tiq:2: error[E09]: vec_pop on a vec with no established element type (no vec_push yet)"

# The typed IR shows TYPE_VEC, parametrized by the established element type.
assert_semantic_ast "typed_vec" 'v = vec_new()
n = vec_push(v, 7)
x = vec_get(v, 0)' 'BINDING v <TYPE_VEC>
  CALL <TYPE_VEC>
    IDENT vec_new
BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT vec_push
    IDENT v <TYPE_VEC:TYPE_INT>
    INT 7 <TYPE_INT>
BINDING x <TYPE_INT>
  CALL <TYPE_INT>
    IDENT vec_get
    IDENT v <TYPE_VEC:TYPE_INT>
    INT 0 <TYPE_INT>'

# M13.1-P4: StrBuf builtins (LANGUAGE_SPEC §19.8).
assert_semantic "strbuf_new_bad_arity" 'sb = str_buf_new(1)
' "$TMP_DIR/strbuf_new_bad_arity.tiq:1: error[E12]: str_buf_new expects no arguments"

assert_semantic "strbuf_append_bad_arity" 'sb = str_buf_new()
n = str_buf_append(sb)
' "$TMP_DIR/strbuf_append_bad_arity.tiq:2: error[E12]: str_buf_append expects exactly 2 arguments"

assert_semantic "strbuf_to_str_bad_arity" 's = str_buf_to_str()
' "$TMP_DIR/strbuf_to_str_bad_arity.tiq:1: error[E12]: str_buf_to_str expects exactly 1 argument"

assert_semantic "strbuf_len_bad_arity" 'sb = str_buf_new()
n = str_buf_len(sb, 1)
' "$TMP_DIR/strbuf_len_bad_arity.tiq:2: error[E12]: str_buf_len expects exactly 1 argument"

assert_semantic "strbuf_append_bad_buf_type" 'n = str_buf_append(1, "a")
' "$TMP_DIR/strbuf_append_bad_buf_type.tiq:1: error[E09]: str_buf_append argument: expected strbuf, found int"

assert_semantic "strbuf_to_str_bad_buf_type" 'x = "s"
s = str_buf_to_str(x)
' "$TMP_DIR/strbuf_to_str_bad_buf_type.tiq:2: error[E09]: str_buf_to_str argument: expected strbuf, found str"

assert_semantic "strbuf_len_bad_buf_type" 'x = 1
n = str_buf_len(x)
' "$TMP_DIR/strbuf_len_bad_buf_type.tiq:2: error[E09]: str_buf_len argument: expected strbuf, found int"

assert_semantic "strbuf_append_bad_value_type" 'sb = str_buf_new()
str_buf_append(sb, 7)
' "$TMP_DIR/strbuf_append_bad_value_type.tiq:2: error[E09]: str_buf_append value: expected str, found int"

# The typed IR shows TYPE_STRBUF for the handle; to_str yields TYPE_STR.
assert_semantic_ast "typed_strbuf" 'sb = str_buf_new()
n = str_buf_append(sb, "hi")
s = str_buf_to_str(sb)
m = str_buf_len(sb)' 'BINDING sb <TYPE_STRBUF>
  CALL <TYPE_STRBUF>
    IDENT str_buf_new
BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT str_buf_append
    IDENT sb <TYPE_STRBUF>
    STRING "hi" <TYPE_STR>
BINDING s <TYPE_STR>
  CALL <TYPE_STR>
    IDENT str_buf_to_str
    IDENT sb <TYPE_STRBUF>
BINDING m <TYPE_INT>
  CALL <TYPE_INT>
    IDENT str_buf_len
    IDENT sb <TYPE_STRBUF>'

# M13.1-P5: Map builtins (LANGUAGE_SPEC §19.9).
assert_semantic "map_new_bad_arity" 'm = map_new(1)
' "$TMP_DIR/map_new_bad_arity.tiq:1: error[E12]: map_new expects no arguments"

assert_semantic "map_set_bad_arity" 'm = map_new()
n = map_set(m, "k")
' "$TMP_DIR/map_set_bad_arity.tiq:2: error[E12]: map_set expects exactly 3 arguments"

assert_semantic "map_get_bad_arity" 'm = map_new()
v = map_get(m)
' "$TMP_DIR/map_get_bad_arity.tiq:2: error[E12]: map_get expects exactly 2 arguments"

assert_semantic "map_has_bad_arity" 'm = map_new()
h = map_has(m, "k", 1)
' "$TMP_DIR/map_has_bad_arity.tiq:2: error[E12]: map_has expects exactly 2 arguments"

assert_semantic "map_len_bad_arity" 'n = map_len()
' "$TMP_DIR/map_len_bad_arity.tiq:1: error[E12]: map_len expects exactly 1 argument"

assert_semantic "map_key_at_bad_arity" 'm = map_new()
k = map_key_at(m)
' "$TMP_DIR/map_key_at_bad_arity.tiq:2: error[E12]: map_key_at expects exactly 2 arguments"

assert_semantic "map_val_at_bad_arity" 'm = map_new()
v = map_val_at(m, 0, 1)
' "$TMP_DIR/map_val_at_bad_arity.tiq:2: error[E12]: map_val_at expects exactly 2 arguments"

assert_semantic "map_set_bad_map_type" 'n = map_set(1, "k", 2)
' "$TMP_DIR/map_set_bad_map_type.tiq:1: error[E09]: map_set argument: expected map, found int"

assert_semantic "map_get_bad_map_type" 'x = "s"
v = map_get(x, "k")
' "$TMP_DIR/map_get_bad_map_type.tiq:2: error[E09]: map_get argument: expected map, found str"

assert_semantic "map_len_bad_map_type" 'x = 1
n = map_len(x)
' "$TMP_DIR/map_len_bad_map_type.tiq:2: error[E09]: map_len argument: expected map, found int"

assert_semantic "map_set_bad_key_type" 'm = map_new()
map_set(m, 1, 2)
' "$TMP_DIR/map_set_bad_key_type.tiq:2: error[E09]: map_set key: expected str, found int"

assert_semantic "map_get_bad_key_type" 'm = map_new()
v = map_get(m, 1)
' "$TMP_DIR/map_get_bad_key_type.tiq:2: error[E09]: map_get key: expected str, found int"

assert_semantic "map_has_bad_key_type" 'm = map_new()
h = map_has(m, 1)
' "$TMP_DIR/map_has_bad_key_type.tiq:2: error[E09]: map_has key: expected str, found int"

assert_semantic "map_set_bad_value_type" 'm = map_new()
map_set(m, "k", "v")
' "$TMP_DIR/map_set_bad_value_type.tiq:2: error[E09]: map_set value: expected int, found str"

assert_semantic "map_key_at_bad_index" 'm = map_new()
k = map_key_at(m, "0")
' "$TMP_DIR/map_key_at_bad_index.tiq:2: error[E09]: map_key_at index: expected int, found str"

assert_semantic "map_val_at_bad_index" 'm = map_new()
v = map_val_at(m, "0")
' "$TMP_DIR/map_val_at_bad_index.tiq:2: error[E09]: map_val_at index: expected int, found str"

# The typed IR shows TYPE_MAP for the handle; get/len/val_at yield TYPE_INT,
# has yields TYPE_BOOL, key_at yields TYPE_STR.
assert_semantic_ast "typed_map" 'm = map_new()
n = map_set(m, "a", 1)
v = map_get(m, "a")
h = map_has(m, "a")
l = map_len(m)
k = map_key_at(m, 0)
w = map_val_at(m, 0)' 'BINDING m <TYPE_MAP>
  CALL <TYPE_MAP>
    IDENT map_new
BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT map_set
    IDENT m <TYPE_MAP>
    STRING "a" <TYPE_STR>
    INT 1 <TYPE_INT>
BINDING v <TYPE_INT>
  CALL <TYPE_INT>
    IDENT map_get
    IDENT m <TYPE_MAP>
    STRING "a" <TYPE_STR>
BINDING h <TYPE_BOOL>
  CALL <TYPE_BOOL>
    IDENT map_has
    IDENT m <TYPE_MAP>
    STRING "a" <TYPE_STR>
BINDING l <TYPE_INT>
  CALL <TYPE_INT>
    IDENT map_len
    IDENT m <TYPE_MAP>
BINDING k <TYPE_STR>
  CALL <TYPE_STR>
    IDENT map_key_at
    IDENT m <TYPE_MAP>
    INT 0 <TYPE_INT>
BINDING w <TYPE_INT>
  CALL <TYPE_INT>
    IDENT map_val_at
    IDENT m <TYPE_MAP>
    INT 0 <TYPE_INT>'

# M13.1-P8: container annotations (LANGUAGE_SPEC §19.10). Malformed vec
# annotations, kind mismatches, nominal element mismatches, borrow
# rejection, return checking, and arity are all fail-closed E09/E12/E23.
assert_semantic "p8_vec_annot_bare" 'f v:vec -> vec_len(v)
' "$TMP_DIR/p8_vec_annot_bare.tiq:1: error[E09]: vec annotation requires an element type: vec[T]"

assert_semantic "p8_vec_annot_unknown_elem" 'f v:vec[wat] -> vec_len(v)
' "$TMP_DIR/p8_vec_annot_unknown_elem.tiq:1: error[E09]: unknown type 'wat'"

assert_semantic "p8_vec_annot_bad_elem" 'f v:vec[bool] -> vec_len(v)
' "$TMP_DIR/p8_vec_annot_bad_elem.tiq:1: error[E09]: vec element type must be int, str, or a struct"

assert_semantic "p8_vec_arg_elem_mismatch" 'f v:vec[int] -> vec_len(v)
v = vec_new()
vec_push(v, "a")
f(v)
' "$TMP_DIR/p8_vec_arg_elem_mismatch.tiq:4: error[E09]: argument 1: expected vec<int>, found vec<str>"

assert_semantic "p8_vec_arg_not_vec" 'f v:vec[int] -> vec_len(v)
f(1)
' "$TMP_DIR/p8_vec_arg_not_vec.tiq:2: error[E09]: argument 1: expected vec<int>, found int"

assert_semantic "p8_container_borrow" 'f v:&vec[int] -> vec_len(v)
' "$TMP_DIR/p8_container_borrow.tiq:1: error[E23]: container parameters are reference-semantics handles; '&' is not allowed"

assert_semantic "p8_vec_return_elem_mismatch" 'g v:vec[str] -> vec[int] -> v
' "$TMP_DIR/p8_vec_return_elem_mismatch.tiq:1: error[E09]: return type mismatch: expected vec<int>, found vec<str>"

assert_semantic "p8_strbuf_arg_mismatch" 'f b:strbuf -> str_buf_len(b)
f(1)
' "$TMP_DIR/p8_strbuf_arg_mismatch.tiq:2: error[E09]: argument 1: expected strbuf, found int"

assert_semantic "p8_vec_fn_arity" 'f v:vec[int] -> vec_len(v)
v = vec_new()
f(v, 2)
' "$TMP_DIR/p8_vec_fn_arity.tiq:3: error[E12]: arity mismatch"

# M13.1-P8: an annotated vec[T] parameter is established inside the callee
# (vec_get works with no prior vec_push), and a vec[T] return annotation
# carries the full element type on the function node.
assert_semantic_ast "p8_typed_vec_param" 'f v:vec[int] -> vec_get(v, 0)
' 'FUNCTION f <TYPE_INT>
  PARAM v
  CALL <TYPE_INT>
    IDENT vec_get
    IDENT v <TYPE_VEC:TYPE_INT>
    INT 0 <TYPE_INT>'

assert_semantic_ast "p8_typed_vec_return" 'f v:vec[int] b:strbuf m:map -> vec[int] -> v
' 'FUNCTION f <TYPE_VEC:TYPE_INT>
  PARAM v
  PARAM b
  PARAM m
  IDENT v <TYPE_VEC:TYPE_INT>'

# M13.1-P9: vec element types compare structurally, not by pooled-pointer
# identity. A user function's return type is interned per-arity
# (type_get_func), so f(1) carries a TYPE_INT instance distinct from the
# canonical one; a vec established via vec_push(v, f(1)) must still be
# accepted by a vec[int] parameter (was a false E09 "expected vec<int>,
# found vec<int>").
assert_semantic_ast "p9_vec_helper_elem_param" 'f x:int -> int -> x + 1
g v:vec[int] -> int -> vec_len(v)
v = vec_new()
vec_push(v, f(1))
n = g(v)
' 'FUNCTION f <TYPE_INT>
  PARAM x
  BINARY PLUS <TYPE_INT>
    IDENT x <TYPE_INT>
    INT 1 <TYPE_INT>
FUNCTION g <TYPE_INT>
  PARAM v
  CALL <TYPE_INT>
    IDENT vec_len
    IDENT v <TYPE_VEC:TYPE_INT>
BINDING v <TYPE_VEC>
  CALL <TYPE_VEC>
    IDENT vec_new
CALL <TYPE_INT>
  IDENT vec_push
  IDENT v <TYPE_VEC:TYPE_INT>
  CALL <TYPE_INT>
    IDENT f <TYPE_INT>
    INT 1 <TYPE_INT>
BINDING n <TYPE_INT>
  CALL <TYPE_INT>
    IDENT g <TYPE_INT>
    IDENT v <TYPE_VEC:TYPE_INT>'

# M13.1-P9: the vec[int] return-annotation check has the same structural
# rule (expression bodies carry the full vec<T> element).
assert_semantic_ast "p9_vec_helper_elem_return" 'h x:int -> int -> x
v = vec_new()
vec_push(v, h(1))
f a:int -> vec[int] -> v
' 'FUNCTION h <TYPE_INT>
  PARAM x
  IDENT x <TYPE_INT>
BINDING v <TYPE_VEC>
  CALL <TYPE_VEC>
    IDENT vec_new
CALL <TYPE_INT>
  IDENT vec_push
  IDENT v <TYPE_VEC:TYPE_INT>
  CALL <TYPE_INT>
    IDENT h <TYPE_INT>
    INT 1 <TYPE_INT>
FUNCTION f <TYPE_VEC:TYPE_INT>
  PARAM a
  IDENT v <TYPE_VEC:TYPE_INT>'

# M13.1-P9 true negative: structural comparison must not over-accept — a
# helper-established vec<int> into a vec[str] parameter is still E09.
assert_semantic "p9_vec_helper_elem_param_neg" 'h x:int -> int -> x
f v:vec[str] -> int -> vec_len(v)
v = vec_new()
vec_push(v, h(1))
f(v)
' "$TMP_DIR/p9_vec_helper_elem_param_neg.tiq:5: error[E09]: argument 1: expected vec<str>, found vec<int>"

# M13.1-P9 true negative: named-struct elements stay nominal — vec<A>
# into a vec[B] parameter is E09 even though the shapes match.
assert_semantic "p9_vec_struct_elem_nominal_neg" 'struct A { x: int }
struct B { x: int }
f v:vec[B] -> int -> vec_len(v)
v = vec_new()
vec_push(v, A { x: 1 })
f(v)
' "$TMP_DIR/p9_vec_struct_elem_nominal_neg.tiq:6: error[E09]: argument 1: expected vec<B>, found vec<A>"

echo "semantic: ok"
