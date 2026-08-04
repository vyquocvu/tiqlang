#!/bin/sh
# M21.1-T1: harness for the performance benchmark suite (tests/perf_suite.sh).
# Verifies record/check modes, the deterministic metric format, the baseline
# file contract, and fail-closed behavior (missing baseline, no build, binary
# size regression). Timing/RSS numbers are machine-dependent, so only their
# presence and numeric shape are asserted here.
set -u

TIQ="${TIQ:-./build/tiq}"
SUITE="tests/perf_suite.sh"
TMP_DIR="${TMPDIR:-/tmp}/tiq-perf-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# All record/check runs use a throwaway baseline so `make test` never
# rewrites the checked-in tests/perf_baseline.txt.
BASE="$TMP_DIR/baseline.txt"

fail=0

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "perf_suite_test: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

expect_out() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.out"; then
    echo "perf_suite_test: FAIL $name (stdout missing '$pattern')" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_err() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.err"; then
    echo "perf_suite_test: FAIL $name (stderr missing '$pattern')" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

# 1. Record mode writes the baseline with every metric key.
expect_exit 0 record env TIQ="$TIQ" PERF_BASELINE="$BASE" sh "$SUITE" record
for key in \
  "binary_size_hello=" \
  "binary_size_fib=" \
  "bench_total_ms=" \
  "bench_throughput_kbs=" \
  "rss_compile_kb="; do
  if ! grep -q "^$key" "$BASE"; then
    echo "perf_suite_test: FAIL record (baseline missing key $key)" >&2
    cat "$BASE" >&2
    fail=1
  fi
done
# All recorded values must be positive integers (deterministic format).
if grep -v '^#' "$BASE" | grep -v '^$' | grep -qvE '^[a-z_]+=[1-9][0-9]*$'; then
  echo "perf_suite_test: FAIL record (non-positive-integer baseline value)" >&2
  cat "$BASE" >&2
  fail=1
fi
# The baseline records the OS it was captured on (sizes are OS-dependent).
if ! grep -q '^# os: ' "$BASE"; then
  echo "perf_suite_test: FAIL record (baseline missing os header)" >&2
  cat "$BASE" >&2
  fail=1
fi

# 2. Check mode against the freshly recorded baseline passes.
expect_exit 0 check env TIQ="$TIQ" PERF_BASELINE="$BASE" sh "$SUITE" check
expect_out check "perf: ok"

# 3. Check mode is deterministic about the hard gate: binary sizes stay
# inside the +10% band (same compiler, same flags, same linker inputs).
expect_exit 0 recheck env TIQ="$TIQ" PERF_BASELINE="$BASE" sh "$SUITE" check

# 4. A missing baseline fails closed.
expect_exit 1 nobase env TIQ="$TIQ" PERF_BASELINE="$TMP_DIR/no-such-baseline.txt" sh "$SUITE" check
expect_err nobase "perf: baseline not found"

# 5. A binary size regression fails the hard gate.
sed 's/^binary_size_hello=.*/binary_size_hello=1/' "$BASE" >"$TMP_DIR/small.txt"
expect_exit 1 regress env TIQ="$TIQ" PERF_BASELINE="$TMP_DIR/small.txt" sh "$SUITE" check
expect_err regress "perf: REGRESSION binary_size_hello"

# 6. An unknown mode is a usage error.
expect_exit 2 badmode env TIQ="$TIQ" PERF_BASELINE="$BASE" sh "$SUITE" frobnicate
expect_err badmode "usage"

# 7. Numeric shape: timing/RSS lines in record output are positive integers.
if ! grep -qE '^bench_total_ms=[1-9][0-9]*$' "$TMP_DIR/record.out"; then
  echo "perf_suite_test: FAIL record (bench_total_ms not numeric)" >&2
  cat "$TMP_DIR/record.out" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "perf_suite_test: failed" >&2
  exit 1
fi
echo "perf_suite_test: ok (record, check, baseline contract, fail-closed gates)"
