#!/bin/sh
# M17.1: IR lowering tests
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-ir-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

assert_ir() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output="$TMP_DIR/$name.ir"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"
  if ! $TIQ dump-ir "$input" > "$output" 2>&1; then
    echo "$name: dump-ir failed" >&2
    cat "$output" >&2
    exit 1
  fi
  printf '%s\n' "$expected" > "$expected_file"
  if ! cmp -s "$expected_file" "$output"; then
    echo "$name: IR mismatch" >&2
    echo "expected:" >&2
    cat "$expected_file" >&2
    echo "actual:" >&2
    cat "$output" >&2
    exit 1
  fi
}

assert_ir_contains() {
  name="$1"
  source="$2"
  pattern="$3"
  input="$TMP_DIR/$name.tiq"
  output="$TMP_DIR/$name.ir"

  printf '%s' "$source" > "$input"
  if ! $TIQ dump-ir "$input" > "$output" 2>&1; then
    echo "$name: dump-ir failed" >&2
    cat "$output" >&2
    exit 1
  fi
  if ! grep -q "$pattern" "$output"; then
    echo "$name: pattern '$pattern' not found in IR" >&2
    cat "$output" >&2
    exit 1
  fi
}

assert_ir_fails() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output="$TMP_DIR/$name.out"
  error="$TMP_DIR/$name.err"

  printf '%s' "$source" > "$input"
  if $TIQ dump-ir "$input" > "$output" 2> "$error"; then
    echo "$name: expected failure" >&2
    exit 1
  fi
  if ! grep -q "$expected" "$error"; then
    echo "$name: expected error '$expected' not found" >&2
    cat "$error" >&2
    exit 1
  fi
}

# Test 1: Simple binding and arithmetic
assert_ir_contains "binding_arith" 'x = 10
y = 20
z = x + y
' "add"

# Test 2: Print builtin
assert_ir_contains "print" 'print(42)
' "print"

# Test 3: Conditional expression
assert_ir_contains "conditional" 'x = 10
y = x > 5 ? 1 : 0
' "cbr"

# Test 4: Range loop
assert_ir_contains "range_loop" '[i <- 0..5] {
  print(i)
}
' "cmp_lt"

# Test 5: Function definition
assert_ir_contains "function_def" 'add a b -> a + b
' "define add"

# Test 6: Function call
assert_ir_contains "function_call" 'add a b -> a + b
x = add(10, 20)
' "call @add"

# Test 7: Array literal
assert_ir_contains "array" 'xs = [1, 2, 3]
' "array_init"

# Test 8: Comparison operators
assert_ir_contains "comparison" 'x = 10
y = 20
b = x < y
' "cmp_lt"

# Test 9: Logical operators
assert_ir_contains "logical" 'a = true
b = false
c = a && b
' "and"

# Test 10: Unary operators
assert_ir_contains "unary" 'x = 10
y = -x
' "neg"

# Test 11: Multiple statements
assert_ir_contains "multi_stmt" 'x = 1
y = 2
z = x + y
print(z)
' "print"

# Test 12: Nested expressions
assert_ir_contains "nested" 'x = (10 + 20) * 30
' "add"

# Test 13: Boolean literals
assert_ir_contains "bool_lit" 'a = true
b = false
' "const_bool"

# Test 14: Integer literals
assert_ir_contains "int_lit" 'x = 42
y = -7
' "const_int"

# Test 15: String literal
assert_ir_contains "str_lit" 'msg = "hello"
' "const_str"

# Test 16: Struct and field access lowering
assert_ir_contains "struct_record" 'struct Point { x: i64, y: i64 }
p = Point { x: 1, y: 2 }
print(p.x)
' "struct_init"

assert_ir_contains "field_access" 'struct Point { x: i64, y: i64 }
p = Point { x: 1, y: 2 }
print(p.y)
' "field_ptr"

# Test 17: Supported examples lower cleanly into IR
supported_examples="examples/arithmetic.tiq
examples/break_early.tiq
examples/continue_skip.tiq
examples/count.tiq
examples/evens.tiq
examples/factorial.tiq
examples/fib.tiq
examples/gcd.tiq
examples/hello.tiq
examples/max.tiq
examples/option_result.tiq
examples/primes.tiq
examples/structs.tiq"

for ex in $supported_examples; do
  if ! $TIQ dump-ir "$ex" > /dev/null 2>&1; then
    echo "example $ex failed to lower" >&2
    exit 1
  fi
done

# Test 17: Semantic errors should fail before IR lowering
assert_ir_fails "undefined_var" 'x = y
' "error\[E08\]"

assert_ir_fails "type_mismatch" 'x = 1 + "foo"
' "error\[E09\]"

echo "IR tests passed"
