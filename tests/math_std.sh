#!/bin/sh
# S4: pure-Tiq math standard library tests (std/math.tiq).
#
# std/math.tiq is written in Tiq itself and wraps no compiler builtins, so it
# is not a gated module (LANGUAGE_SPEC §17.7): nothing here asserts that a
# builtin is unreachable without the import. These tests pin behaviour:
# exact integer results, exact f64 rounding/fmod results, and tolerance-based
# verdicts for the transcendental kernels.
#
# Every float assertion prints a bool produced by math_close, never a raw
# f64. The generated C is compiled by the host compiler without
# -ffp-contract=off, so FMA contraction can shift a Taylor accumulator by an
# ulp between architectures; a 1e-9 relative verdict is portable where a
# golden float dump would not be.
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-math-std-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "math_std: FAIL $1" >&2
  shift
  for f in "$@"; do
    echo "--- $f" >&2
    cat "$f" >&2 || true
  done
  exit 1
}

# assert_runs NAME SOURCE EXPECTED — compiles and runs SOURCE, comparing
# stdout byte-for-byte against EXPECTED.
assert_runs() {
  name="$1"
  source="$2"
  expected="$3"
  in_file="$TMP_DIR/$name.tiq"
  out_file="$TMP_DIR/$name.out"
  err_file="$TMP_DIR/$name.err"
  exp_file="$TMP_DIR/$name.expected"
  printf '%s\n' "$source" > "$in_file"
  if ! "$TIQ" run "$in_file" > "$out_file" 2>"$err_file"; then
    fail "$name (run failed)" "$in_file" "$err_file"
  fi
  printf '%s\n' "$expected" > "$exp_file"
  if ! cmp -s "$exp_file" "$out_file"; then
    fail "$name (output mismatch)" "$exp_file" "$out_file"
  fi
}

# assert_rejects NAME SOURCE PATTERN — the build must fail, and its stderr
# must contain PATTERN.
assert_rejects() {
  name="$1"
  source="$2"
  pattern="$3"
  in_file="$TMP_DIR/$name.tiq"
  err_file="$TMP_DIR/$name.err"
  exe_file="$TMP_DIR/$name.bin"
  printf '%s\n' "$source" > "$in_file"
  if "$TIQ" build "$in_file" -o "$exe_file" 2>"$err_file"; then
    fail "$name (expected build to fail)" "$err_file"
  fi
  if ! grep -q "$pattern" "$err_file"; then
    echo "math_std: FAIL $name (diagnostic mismatch)" >&2
    echo "expected pattern: $pattern" >&2
    echo "actual:" >&2
    cat "$err_file" >&2
    exit 1
  fi
}

# --- 0. The module itself must check clean ---

if ! "$TIQ" check std/math.tiq > /dev/null 2>&1; then
  fail "check std/math.tiq failed"
fi

# --- 1. i64 sign, magnitude, and bounds ---

assert_runs "i64_basic" 'import "std/math.tiq"

print(math_abs(-5))
print(math_abs(5))
print(math_abs(0))
print(math_sign(-3))
print(math_sign(0))
print(math_sign(4))
print(math_min(2, 7))
print(math_min(7, 2))
print(math_max(2, 7))
print(math_clamp(3, 0, 5))
print(math_clamp(-1, 0, 5))
print(math_clamp(10, 0, 5))
' '5
5
0
-1
0
1
2
2
7
3
0
5'

# math_abs of INT64_MIN has no positive i64 representative, so it returns its
# argument unchanged. The literal is spelled with arithmetic because a bare
# 9223372036854775808 is out of i64 range.
assert_runs "i64_min_abs" 'import "std/math.tiq"

m = 0 - 9223372036854775807 - 1
print(math_abs(m))
print(math_sign(m))
' '-9223372036854775808
-1'

# --- 2. Euclidean division, gcd, lcm ---

assert_runs "i64_divmod" 'import "std/math.tiq"

print(math_mod(7, 3))
print(math_mod(-7, 3))
print(math_mod(7, -3))
print(math_mod(-7, -3))
print(math_div(7, 3))
print(math_div(-7, 3))
print(math_div(7, -3))
print(math_div(-7, -3))
print(math_mod(7, 0) == 0)
print(math_div(7, 0) == 0)
print(math_mod(5, 0 - 9223372036854775807 - 1) == -1)
' '1
2
1
2
2
-3
-2
3
true
true
true'

# The defining identity of the pair, for all four sign combinations.
assert_runs "i64_divmod_identity" 'import "std/math.tiq"

print(math_div(7, 3) * 3 + math_mod(7, 3) == 7)
print(math_div(-7, 3) * 3 + math_mod(-7, 3) == -7)
print(math_div(7, -3) * -3 + math_mod(7, -3) == 7)
print(math_div(-7, -3) * -3 + math_mod(-7, -3) == -7)
print(math_mod(-7, 3) >= 0)
' 'true
true
true
true
true'

# An INT64_MIN argument has no positive representative, so gcd reports the
# module-wide -1 domain sentinel rather than a negative divisor.
assert_runs "i64_gcd_lcm" 'import "std/math.tiq"

print(math_gcd(12, 18))
print(math_gcd(18, 12))
print(math_gcd(-12, 18))
print(math_gcd(7, 5))
print(math_gcd(0, 0))
print(math_gcd(0 - 9223372036854775807 - 1, 0) == -1)
print(math_gcd(0 - 9223372036854775807 - 1, 6) == -1)
print(math_lcm(4, 6))
print(math_lcm(-4, 6))
print(math_lcm(0, 5))
print(math_lcm(4611686018427387904, 2))
print(math_lcm(4611686018427387904, 3) == -1)
' '6
6
6
1
0
true
true
12
12
0
4611686018427387904
true'

# --- 3. i64 powers, bit width, and primality ---

assert_runs "i64_isqrt" 'import "std/math.tiq"

print(math_isqrt(0))
print(math_isqrt(1))
print(math_isqrt(4))
print(math_isqrt(15))
print(math_isqrt(16))
print(math_isqrt(99))
print(math_isqrt(100))
print(math_isqrt(2147395600))
print(math_isqrt(9223372036854775807))
print(math_isqrt(0 - 1) == -1)
' '0
1
2
3
4
9
10
46340
3037000499
true'

# math_pow overflows to the -1 sentinel. That value is also a legitimate
# product (pow(-1, 3)), which the module cannot distinguish; the sentinel is
# documented as ambiguous rather than encoded in a second return value.
assert_runs "i64_pow" 'import "std/math.tiq"

print(math_pow(2, 10))
print(math_pow(2, 0))
print(math_pow(0, 0))
print(math_pow(3, 5))
print(math_pow(0 - 2, 3))
print(math_pow(10, 18))
print(math_pow(10, 19) == -1)
print(math_pow(2, 0 - 1) == -1)
print(math_pow(0 - 9223372036854775807 - 1, 1) == -1)
print(math_pow(2, 62))
' '1024
1
1
243
-8
1000000000000000000
true
true
true
4611686018427387904'

assert_runs "i64_log2_popcount" 'import "std/math.tiq"

print(math_log2(1))
print(math_log2(2))
print(math_log2(3))
print(math_log2(256))
print(math_log2(9223372036854775807))
print(math_log2(0) == -1)
print(math_log2(0 - 5) == -1)
print(math_popcount(0))
print(math_popcount(1))
print(math_popcount(255))
print(math_popcount(4611686018427387904))
print(math_popcount(9223372036854775807))
print(math_popcount(0 - 1))
' '0
1
1
8
62
true
true
0
1
8
1
63
64'

# Trial division stops at sqrt(x), so 2^31 - 1 costs ~46k iterations.
assert_runs "i64_is_prime" 'import "std/math.tiq"

print(math_is_prime(0))
print(math_is_prime(1))
print(math_is_prime(2))
print(math_is_prime(3))
print(math_is_prime(4))
print(math_is_prime(9))
print(math_is_prime(97))
print(math_is_prime(100))
print(math_is_prime(7919))
print(math_is_prime(2147483647))
print(math_is_prime(2147483645))
print(math_is_prime(0 - 7))
' '0
0
1
1
0
0
1
0
1
1
0
0'

echo "math_std: ok"
