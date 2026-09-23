#!/bin/sh
# Native backend integration tests
# Tests the SSA IR -> AMD64 native code generator
set -eu

TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-native-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# Test program execution with native backend
# Note: build_native only works on Linux x86_64
# On other platforms, we test dump-native (assembly generation) only

test_native_run() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output="$TMP_DIR/$name.out"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"

  # Try to build and run with native backend
  if ! $TIQ run --backend native "$input" > "$output" 2>&1; then
    echo "$name: native run failed" >&2
    cat "$output" >&2
    exit 1
  fi

  printf '%s\n' "$expected" > "$expected_file"
  if ! cmp -s "$expected_file" "$output"; then
    echo "$name: output mismatch" >&2
    echo "expected:" >&2
    cat "$expected_file" >&2
    echo "actual:" >&2
    cat "$output" >&2
    exit 1
  fi
}

test_native_dump() {
  name="$1"
  source="$2"
  pattern="$3"
  input="$TMP_DIR/$name.tiq"
  output="$TMP_DIR/$name.asm"

  printf '%s' "$source" > "$input"

  if ! $TIQ dump-native "$input" > "$output" 2>&1; then
    echo "$name: dump-native failed" >&2
    cat "$output" >&2
    exit 1
  fi

  if ! grep -q "$pattern" "$output"; then
    echo "$name: pattern '$pattern' not found in assembly" >&2
    cat "$output" >&2
    exit 1
  fi
}

# Test 1: Simple arithmetic
test_native_dump "arith" 'x = 10 + 20
print(x)
' "movq"

# Test 2: Function definition and call
test_native_dump "func" 'add a: i64 b: i64 : i64 -> a + b
x = add(10, 20)
print(x)
' "tiq_fn_add"

# Test 3: Conditional expression (use function parameter to prevent constant folding)
test_native_dump "conditional" 'test x: i64 -> x > 5 ? 1 : 0
y = test(10)
print(y)
' "testq"

# Test 4: Comparison operators (use function parameters to prevent constant folding)
test_native_dump "comparison" 'test a: i64 b: i64 -> a < b
b = test(10, 20)
print(b)
' "cmpq"

# Test 5: Loop with mutable binding
test_native_dump "loop" 'sum <- 0
[i <- 1..6] {
  sum <- sum + i
}
print(sum)
' "jmp"

# Test 6: Multiple functions
test_native_dump "multi_func" 'double x: i64 : i64 -> x * 2
triple x: i64 : i64 -> x * 3
x = double(5)
y = triple(3)
print(x)
print(y)
' "tiq_fn_double"

# Test 7: Boolean operations (use function parameters to prevent constant folding)
test_native_dump "bool_ops" 'test a: bool b: bool -> a && b
c = test(true, false)
print(c)
' "andq"

# Test 8: String literal
test_native_dump "string" 'msg = "hello"
print(msg)
' "tiq_print_str"

# Test 9: Negative numbers (use function parameter to prevent constant folding)
test_native_dump "negative" 'negate x: i64 -> -x
y = negate(42)
print(y)
' "negq"

# Test 10: Nested expressions
test_native_dump "nested" 'x = (10 + 20) * 30
print(x)
' "movq"

# Platform-specific tests: actual execution
# Only run on Linux x86_64 where build_native is supported
if [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "x86_64" ]; then
  echo "Running native execution tests on Linux x86_64..."

  test_native_run "arith_exec" 'x = 10 + 20
print(x)
' "30"

  test_native_run "func_exec" 'add a: i64 b: i64 : i64 -> a + b
x = add(10, 20)
print(x)
' "30"

  test_native_run "conditional_exec" 'max a: i64 b: i64 : i64 -> a > b ? a : b
x = max(10, 20)
print(x)
y = max(30, 15)
print(y)
' "20
30"

  test_native_run "loop_exec" 'sum <- 0
[i <- 1..6] {
  sum <- sum + i
}
print(sum)
' "15"

  test_native_run "multi_func_exec" 'double x: i64 : i64 -> x * 2
triple x: i64 : i64 -> x * 3
x = double(5)
y = triple(3)
print(x)
print(y)
' "10
9"
else
  echo "Skipping native execution tests (not on Linux x86_64)"
fi

echo "Native backend tests passed"
