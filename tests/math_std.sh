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
CC_BIN="${CC:-cc}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-math-std-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# SAN is 1 when the host compiler has ASan and UBSan. The totality group below
# loses most of its meaning without them, so a host without them says so out
# loud instead of passing quietly.
SAN=1
printf 'int main(void){return 0;}\n' > "$TMP_DIR/sanprobe.c"
if ! "$CC_BIN" -std=c11 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
     "$TMP_DIR/sanprobe.c" -o "$TMP_DIR/sanprobe" 2>/dev/null; then
  SAN=0
  echo "math_std: note - host cc has no ASan/UBSan, running group 9 unsanitized" >&2
fi

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

# run_guarded EXE OUT SECS — run EXE with stdout in OUT and stderr in OUT.log,
# and kill it if it is still alive after SECS. The deadline is a sleep loop
# because `timeout` is not portable to the macOS hosts this suite runs on, and
# an uncapped kernel loop would otherwise wedge `make test` indefinitely. Sets
# GUARDED_RC and GUARDED_TIMEDOUT.
run_guarded() {
  exe="$1"
  out="$2"
  secs="$3"
  deadline="$out.deadline"
  rm -f "$deadline"
  GUARDED_TIMEDOUT=0
  "$exe" >"$out" 2>"$out.log" &
  pid=$!
  (
    n=0
    while [ "$n" -lt "$secs" ]; do
      kill -0 "$pid" 2>/dev/null || exit 0
      sleep 1
      n=$((n + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null
      : >"$deadline"
    fi
  ) &
  wpid=$!
  GUARDED_RC=0
  wait "$pid" || GUARDED_RC=$?
  kill "$wpid" 2>/dev/null || true
  wait "$wpid" 2>/dev/null || true
  if [ -f "$deadline" ]; then
    GUARDED_TIMEDOUT=1
  fi
}

# assert_sanitized NAME SOURCE EXPECTED — emit C for SOURCE, compile it with
# ASan plus non-recovering UBSan, and run it under the watchdog. Groups 1-8
# pin values through `tiq run`, which cannot see the two failure modes this
# path is for: an out-of-range f64 -> i64 conversion saturates on arm64 and
# yields LLONG_MIN on x86-64, so only UBSan reports it at all, and a loop
# without a cap looks like a slow test rather than a hang.
assert_sanitized() {
  name="$1"
  source="$2"
  expected="$3"
  src="$TMP_DIR/$name.tiq"
  cfile="$TMP_DIR/$name.c"
  bin="$TMP_DIR/$name.san"
  out="$TMP_DIR/$name.out"
  exp="$TMP_DIR/$name.expected"
  cerr="$TMP_DIR/$name.emit.err"
  ccerr="$TMP_DIR/$name.cc.err"
  printf '%s\n' "$source" > "$src"
  if ! "$TIQ" emit-c "$src" > "$cfile" 2>"$cerr"; then
    fail "$name (emit-c failed)" "$cerr"
  fi
  sanflags="-fsanitize=address,undefined -fno-sanitize-recover=all"
  [ "$SAN" = "1" ] || sanflags=""
  # shellcheck disable=SC2086
  if ! "$CC_BIN" -std=c11 -g -O1 $sanflags "$cfile" -o "$bin" 2>"$ccerr"; then
    fail "$name (sanitizer compile failed)" "$ccerr"
  fi
  run_guarded "$bin" "$out" 60
  if [ "$GUARDED_TIMEDOUT" = "1" ]; then
    fail "$name (watchdog: no result in 60s)" "$src" "$out" "$out.log"
  fi
  if [ "$GUARDED_RC" != "0" ]; then
    fail "$name (sanitized run exited $GUARDED_RC)" "$src" "$out.log"
  fi
  printf '%s\n' "$expected" > "$exp"
  if ! cmp -s "$exp" "$out"; then
    fail "$name (output mismatch)" "$exp" "$out" "$out.log"
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

# --- 4. f64 constants and predicates ---

# The constants are pinned against their own literals: both sides are the same
# decimal, so the comparison is exact and needs no tolerance.
assert_runs "f64_const" 'import "std/math.tiq"

pi = math_const(MathConst.Pi)
print(math_const(MathConst.Pi) == pi)
print(math_const(MathConst.Tau) == pi * 2.0)
print(math_const(MathConst.E) == 2.718281828459045)
print(math_const(7) == 0.0)
print(math_const(0 - 1) == 0.0)
' 'true
true
true
true
true'

assert_runs "f64_predicates" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
nan = inf / inf
print(math_is_nan(1.0))
print(math_is_nan(nan))
print(math_is_finite(1.0))
print(math_is_finite(0.0))
print(math_is_finite(inf))
print(math_is_finite(nan))
print(math_fabs(0.0 - 2.5))
print(math_fabs(2.5))
print(math_fabs(0.0))
print(math_fabs(nan) == nan)
' 'false
true
true
true
false
false
2.5
2.5
0
false'

# math_close is the test oracle for every tolerance case below, so it is
# pinned here. Tiq has no scientific-notation literals, hence the spelled-out
# tolerances.
assert_runs "f64_close" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
print(math_close(0.1 + 0.2, 0.3))
print(math_close(1.0, 1.0000000001))
print(math_close(1.0, 1.0000000000000002))
print(math_close(0.0, 0.0000000000001))
print(math_close(0.0, 0.00000000001))
print(math_close(inf, inf))
print(math_close(inf, 1.0))
print(math_close(1.0, inf))
print(math_close(0.0 - 1.0, 1.0))
print(math_close(1000000.0, 1000000.0000001))
' 'true
false
true
true
false
true
false
false
false
true'

# --- 5. f64 scaling and rounding ---

# 2 ** 1024 overflows f64, so the positive range stops at 1023 and anything
# outside [-1074, 1023] returns the 0.0 sentinel. Each comparison is between
# exactly representable doubles, so == is the right predicate and no tolerance
# is involved.
assert_runs "f64_two_pow" 'import "std/math.tiq"

print(math_two_pow(0) == 1.0)
print(math_two_pow(10) == 1024.0)
print(math_two_pow(0 - 1) == 0.5)
print(math_two_pow(0 - 10) == 0.0009765625)
print(math_two_pow(63) == 9223372036854775808.0)
print(math_two_pow(1023) * math_two_pow(0 - 1023) == 1.0)
print(math_two_pow(0 - 1074) > 0.0)
print(math_two_pow(0 - 1075) == 0.0)
print(math_two_pow(1024) == 0.0)
print(math_two_pow(9223372036854775807) == 0.0)
print(math_two_pow(0 - 9223372036854775807 - 1) == 0.0)
' 'true
true
true
true
true
true
true
true
true
true
true'

assert_runs "f64_round" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
ninf = 0.0 - inf
nan = inf / inf
big = 9007199254740992.0
print(math_trunc(2.5) == 2.0)
print(math_trunc(0.0 - 2.5) == 0.0 - 2.0)
print(math_floor(2.9) == 2.0)
print(math_floor(0.0 - 2.1) == 0.0 - 3.0)
print(math_floor(3.0) == 3.0)
print(math_ceil(2.1) == 3.0)
print(math_ceil(0.0 - 2.9) == 0.0 - 2.0)
print(math_ceil(3.0) == 3.0)
print(math_round(0.5) == 1.0)
print(math_round(1.5) == 2.0)
print(math_round(2.5) == 3.0)
print(math_round(0.0 - 2.5) == 0.0 - 3.0)
print(math_round(0.49999999999999994) == 0.0)
print(math_floor(big) == big)
print(math_ceil(big) == big)
print(math_trunc(inf) == inf)
print(math_floor(inf) == inf)
print(math_ceil(inf) == inf)
print(math_round(inf) == inf)
print(math_trunc(ninf) == ninf)
print(math_floor(ninf) == ninf)
print(math_is_nan(math_trunc(nan)))
print(math_is_nan(math_floor(nan)))
print(math_is_nan(math_ceil(nan)))
print(math_is_nan(math_round(nan)))
' 'true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true'

# --- 6. f64 square root and remainder ---

assert_runs "f64_sqrt" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
nan = inf / inf
print(math_sqrt(0.0) == 0.0)
print(math_sqrt(1.0) == 1.0)
print(math_sqrt(4.0) == 2.0)
print(math_sqrt(0.25) == 0.5)
print(math_sqrt(9.0) == 3.0)
print(math_sqrt(16.0) == 4.0)
print(math_close(math_sqrt(2.0), 1.4142135623730951))
print(math_close(math_sqrt(3.0), 1.7320508075688772))
print(math_close(math_sqrt(0.00000001), 0.0001))
print(math_close(math_sqrt(inf), inf))
print(math_sqrt(0.0 - 1.0) == 0.0)
print(math_sqrt(nan) == 0.0)
' 'true
true
true
true
true
true
true
true
true
true
true
true'

# Every fmod result below is exact in binary floating point, so the pin is an
# equality rather than a tolerance.
assert_runs "f64_fmod" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
print(math_fmod(7.5, 2.0) == 1.5)
print(math_fmod(0.0 - 7.5, 2.0) == 0.0 - 1.5)
print(math_fmod(7.5, 0.0 - 2.0) == 1.5)
print(math_fmod(5.0, 5.0) == 0.0)
print(math_fmod(1.0, 3.0) == 1.0)
print(math_fmod(0.0, 3.0) == 0.0)
print(math_fmod(100.0, 7.0) == 2.0)
print(math_close(math_fmod(0.3, 0.1), 0.09999999999999998))
print(math_fmod(7.5, 0.0) == 0.0)
print(math_fmod(inf, 2.0) == 0.0)
' 'true
true
true
true
true
true
true
true
true
true'

# --- 7. f64 exponentials and logarithms ---

assert_runs "f64_exp_ln" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
print(math_exp(0.0) == 1.0)
print(math_close(math_exp(1.0), 2.718281828459045))
print(math_close(math_exp(0.0 - 1.0), 0.36787944117144233))
print(math_close(math_exp(10.0), 22026.465794806718))
print(math_close(math_exp(0.0 - 10.0), 0.00004539992976248485))
print(math_close(math_exp(700.0) / math_exp(699.0), 2.718281828459045))
print(math_exp(709.5) == 0.0)
print(math_exp(0.0 - 745.5) == 0.0)
print(math_exp(inf) == 0.0)
print(math_ln(1.0) == 0.0)
print(math_close(math_ln(2.0), 0.6931471805599453))
print(math_close(math_ln(math_const(MathConst.E)), 1.0))
print(math_close(math_ln(0.5), 0.0 - 0.6931471805599453))
print(math_close(math_ln(math_exp(3.0)), 3.0))
print(math_ln(0.0) == 0.0)
print(math_ln(0.0 - 2.0) == 0.0)
print(math_ln(inf) == 0.0)
' 'true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true'

assert_runs "f64_powf" 'import "std/math.tiq"

print(math_close(math_powf(2.0, 10.0), 1024.0))
print(math_close(math_powf(9.0, 0.5), 3.0))
print(math_powf(2.0, 0.0) == 1.0)
print(math_close(math_powf(1.0, 100.0), 1.0))
print(math_close(math_powf(2.718281828459045, 2.0), 7.389056098930649))
print(math_powf(0.0 - 2.0, 2.0) == 0.0)
print(math_powf(0.0, 2.0) == 0.0)
' 'true
true
true
true
true
true
true'

# --- 8. f64 trigonometry and geometry ---

# Argument reduction is naive (subtract a rounded count of pi/2), so these
# pins stay within about |x| <= 100 where the reduction error is below the
# 1e-12 tolerance the module claims.
assert_runs "f64_trig" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
pi = math_const(MathConst.Pi)
hpi = pi / 2.0
print(math_close(math_sin(0.0), 0.0))
print(math_close(math_sin(hpi), 1.0))
print(math_close(math_sin(pi), 0.0))
print(math_close(math_sin(0.0 - hpi), 0.0 - 1.0))
print(math_close(math_sin(0.5), 0.479425538604203))
print(math_close(math_sin(100.0), 0.0 - 0.5063656411102649))
print(math_close(math_sin(inf), 0.0))
print(math_close(math_cos(0.0), 1.0))
print(math_close(math_cos(hpi), 0.0))
print(math_close(math_cos(pi), 0.0 - 1.0))
print(math_close(math_cos(2.0 * pi), 1.0))
print(math_close(math_cos(0.5), 0.8775825618903728))
print(math_close(math_cos(100.0), 0.8623188722876839))
print(math_close(math_tan(0.0), 0.0))
print(math_close(math_tan(0.5), 0.5463024898437905))
print(math_close(math_tan(0.0 - 0.5), 0.0 - 0.5463024898437905))
print(math_close(math_atan(0.0), 0.0))
print(math_close(math_atan(0.5), 0.4636476090008061))
print(math_close(math_atan(1.0), pi / 4.0))
print(math_close(math_atan(0.0 - 1.0), 0.0 - pi / 4.0))
print(math_close(math_atan(1000.0), 1.5697963271282298))
print(math_close(math_atan(0.0 - 1000.0), 0.0 - 1.5697963271282298))
print(math_close(math_atan2(1.0, 1.0), 0.7853981633974483))
print(math_close(math_atan2(1.0, 0.0 - 1.0), 2.356194490192345))
print(math_close(math_atan2(0.0 - 1.0, 0.0 - 1.0), 0.0 - 2.356194490192345))
print(math_close(math_atan2(1.0, 0.0), hpi))
print(math_close(math_atan2(0.0 - 1.0, 0.0), 0.0 - hpi))
print(math_atan2(0.0, 0.0) == 0.0)
print(math_close(math_hypot(3.0, 4.0), 5.0))
print(math_close(math_hypot(0.0 - 3.0, 0.0 - 4.0), 5.0))
print(math_hypot(0.0, 0.0) == 0.0)
print(math_close(math_deg(hpi), 90.0))
print(math_close(math_rad(90.0), hpi))
' 'true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true'

# --- 9. Totality under sanitizers: extremes, NaN, infinity ---

# These three programs pin the module-wide totality policy: every entry point
# returns a determinate value for every representable argument, and the walk
# there invokes no undefined behaviour. The i64 domain sentinel is -1 and the
# f64 sentinel is 0.0; the rounding family propagates NaN and infinity because
# it defaults to its argument before any conversion.

assert_sanitized "total_i64_extremes" 'import "std/math.tiq"

m = 0 - 9223372036854775807 - 1
M = 9223372036854775807

print(math_abs(m) == m)
print(math_sign(m) == -1)
print(math_min(m, M) == m)
print(math_max(m, M) == M)
print(math_clamp(m, 0, 5) == 0)
print(math_clamp(M, 0, 5) == 5)
print(math_mod(7, m) == -1)
print(math_div(7, m) == -1)
print(math_mod(m, m) == -1)
print(math_mod(m, 3) == 1)
print(math_div(m, 3) == -3074457345618258603)
print(math_mod(M, 3) == 1)
print(math_gcd(m, 0) == -1)
print(math_gcd(0, m) == -1)
print(math_lcm(m, 5) == -1)
print(math_pow(m, 2) == -1)
print(math_pow(2, 63) == -1)
print(math_pow(2, 62) == 4611686018427387904)
print(math_isqrt(-1) == -1)
print(math_isqrt(M) == 3037000499)
print(math_log2(0) == -1)
print(math_log2(m) == -1)
print(math_log2(M) == 62)
print(math_popcount(m) == 1)
print(math_popcount(-1) == 64)
print(math_is_prime(m) == 0)
print(math_is_prime(2147483647) == 1)
' 'true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true'

assert_sanitized "total_nan_inf" 'import "std/math.tiq"

keep n:i64 : i64 -> { n }

inf = 1.0 / f64(keep(0))
nan = inf / inf
ninf = 0.0 - inf

print(math_is_nan(math_fabs(nan)))
print(math_fabs(ninf) == inf)
print(math_is_nan(nan))
print(!math_is_finite(nan))
print(!math_is_finite(inf))
print(math_is_finite(1.0))
print(!math_close(nan, nan))
print(math_close(inf, inf))
print(math_is_nan(math_trunc(nan)))
print(math_is_nan(math_floor(nan)))
print(math_is_nan(math_ceil(nan)))
print(math_is_nan(math_round(nan)))
print(math_trunc(inf) == inf)
print(math_trunc(ninf) == ninf)
print(math_floor(inf) == inf)
print(math_ceil(inf) == inf)
print(math_round(inf) == inf)
print(math_sqrt(nan) == 0.0)
print(math_sqrt(ninf) == 0.0)
print(math_sqrt(inf) == inf)
print(math_fmod(nan, 2.0) == 0.0)
print(math_fmod(2.0, nan) == 0.0)
print(math_fmod(inf, 2.0) == 0.0)
print(math_fmod(5.0, inf) == 0.0)
print(math_ln(nan) == 0.0)
print(math_ln(inf) == 0.0)
print(math_ln(ninf) == 0.0)
print(math_ln(0.0) == 0.0)
print(math_ln(0.0 - 2.0) == 0.0)
print(math_exp(nan) == 0.0)
print(math_exp(inf) == 0.0)
print(math_exp(ninf) == 0.0)
print(math_powf(nan, 2.0) == 0.0)
print(math_powf(inf, 2.0) == 0.0)
print(math_powf(0.0 - 2.0, 2.0) == 0.0)
print(math_sin(nan) == 0.0)
print(math_cos(inf) == 0.0)
print(math_tan(ninf) == 0.0)
print(math_atan(inf) == 0.0)
print(math_atan2(inf, 1.0) == 0.0)
print(math_atan2(1.0, nan) == 0.0)
print(math_hypot(nan, 1.0) == 0.0)
print(math_hypot(1.0, inf) == 0.0)
print(math_is_nan(math_deg(nan)))
print(math_deg(inf) == inf)
print(math_rad(ninf) == ninf)
' 'true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true'

assert_sanitized "total_edge_finite" 'import "std/math.tiq"

pi = math_const(MathConst.Pi)
huge = math_two_pow(1023)
tiny = math_two_pow(-1074)
b52 = 4503599627370496.0

print(huge > 0.0 && math_is_finite(huge))
print(math_two_pow(1024) == 0.0)
print(math_two_pow(-1075) == 0.0)
print(math_two_pow(-1074) > 0.0)
print(math_two_pow(1023) * math_two_pow(-1023) == 1.0)
print(math_trunc(huge) == huge)
print(math_floor(huge) == huge)
print(math_ceil(huge) == huge)
print(math_round(huge) == huge)
print(math_hypot(huge, 1.0) == huge)
print(math_sqrt(math_two_pow(1022)) == math_two_pow(511))
print(math_sqrt(tiny) == math_two_pow(-537))
print(math_fmod(huge, 1.0) == 0.0)
print(math_fmod(1.0, tiny) == 0.0)
print(math_fmod(tiny, 1.0) == tiny)
print(math_exp(huge) == 0.0)
print(math_exp(0.0 - huge) == 0.0)
print(math_close(math_ln(huge), 709.0895657128240))
print(math_close(math_ln(tiny), -744.4400719213812))
print(math_powf(huge, 0.0) == 1.0)
print(math_powf(tiny, 0.0) == 1.0)
print(math_is_finite(math_sin(b52)))
print(math_is_finite(math_cos(b52)))
print(math_close(math_atan(huge), pi / 2.0))
print(math_close(math_atan(0.0 - huge), 0.0 - pi / 2.0))
print(math_tan(huge) == 0.0)
print(math_sin(huge) == 0.0)
print(math_cos(huge) == 0.0)
' 'true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true'

echo "math_std: ok"
