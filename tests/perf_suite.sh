#!/bin/sh
# M21.1: continuous performance benchmark suite. Measures the deterministic
# and machine-dependent metrics of the compiler and its output binaries, and
# gates against a checked-in baseline (tests/perf_baseline.txt).
#
# Metrics:
#   binary_size_hello / binary_size_fib  — bytes of `tiq build` output for
#       examples/hello.tiq and examples/fib.tiq. Deterministic for a fixed
#       host C toolchain and OS, so these are the hard regression gate
#       (growth beyond +10% fails; toolchain drift stays inside the band).
#   bench_total_ms / bench_throughput_kbs — self-hosted front-end timing
#       (`tiq bench -i 5` over the largest compiler slice). Machine-dependent:
#       reported, soft-warned on large regressions, never failed on.
#   rss_compile_kb — peak resident set size of compiling the largest compiler
#       slice. Machine-dependent like the bench numbers.
#
# Modes:
#   record — measure and write the baseline file (make perf-record)
#   check  — measure and compare against the baseline (make perf-check)
#
# The baseline carries a `# os: <uname -s>` header: binary formats differ
# between Mach-O and ELF, so the hard size gate applies only when the current
# OS matches the recording OS; a mismatch downgrades to a soft check that
# still fails closed on missing or non-positive metrics.
#
# Exit codes: 0 ok, 1 failure (missing baseline, regression, measurement
# error), 2 usage error. Fail-closed throughout: an unmeasurable metric is a
# failure, never a placeholder value.

set -u

TIQ="${TIQ:-./build/tiq}"
BASELINE="${PERF_BASELINE:-tests/perf_baseline.txt}"
BENCH_ITER="${PERF_BENCH_ITER:-5}"
# The largest flattened compiler slice — the standing Phase-4 workload
# (docs/OPTIMIZATION_PLAN.md M14.3 baseline uses the same file).
WORKLOAD="src/tiq/emit_c_main.tiq"

TMP="${TMPDIR:-/tmp}/tiq-perf-suite-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

usage() {
  echo "usage: perf_suite.sh record|check" >&2
  exit 2
}

die() {
  echo "perf: $1" >&2
  exit 1
}

# Peak RSS in KB of a command. macOS `/usr/bin/time -l` reports bytes;
# Linux GNU `/usr/bin/time -v` reports kbytes. Anything else fails closed.
measure_rss_kb() {
  case "$(uname -s)" in
  Darwin)
    out=$(/usr/bin/time -l "$@" 2>&1 >/dev/null) || return 1
    bytes=$(printf '%s\n' "$out" | awk '/maximum resident set size/ { print $1; exit }')
    [ -n "$bytes" ] || return 1
    echo $((bytes / 1024))
    ;;
  Linux)
    out=$(/usr/bin/time -v "$@" 2>&1 >/dev/null) || return 1
    kb=$(printf '%s\n' "$out" | awk -F': ' '/Maximum resident set size/ { print $2; exit }')
    [ -n "$kb" ] || return 1
    echo "$kb"
    ;;
  *)
    return 1
    ;;
  esac
}

positive_int() {
  case "$1" in
  '' | *[!0-9]*) return 1 ;;
  esac
  [ "$1" -gt 0 ] || return 1
  return 0
}

[ $# -eq 1 ] || usage

[ -x "$TIQ" ] || die "compiler not found: $TIQ (run make first)"
[ -f "$WORKLOAD" ] || die "workload not found: $WORKLOAD"

# --- measurement ---------------------------------------------------------

measure() {
  if ! "$TIQ" build examples/hello.tiq -o "$TMP/hello" 2>"$TMP/err"; then
    cat "$TMP/err" >&2
    die "cannot build examples/hello.tiq"
  fi
  if ! "$TIQ" build examples/fib.tiq -o "$TMP/fib" 2>"$TMP/err"; then
    cat "$TMP/err" >&2
    die "cannot build examples/fib.tiq"
  fi
  M_HELLO=$(wc -c <"$TMP/hello" | tr -d ' ')
  M_FIB=$(wc -c <"$TMP/fib" | tr -d ' ')

  if ! "$TIQ" build src/tiq/tools/bench.tiq -o "$TMP/tiq-bench" 2>"$TMP/err"; then
    cat "$TMP/err" >&2
    die "cannot build src/tiq/tools/bench.tiq"
  fi
  if ! "$TMP/tiq-bench" -i "$BENCH_ITER" "$WORKLOAD" >"$TMP/bench.out" 2>"$TMP/err"; then
    cat "$TMP/err" >&2
    die "bench workload failed"
  fi
  M_TOTAL=$(awk '/^  total:/ { print $2; exit }' "$TMP/bench.out")
  positive_int "$M_TOTAL" || die "bench produced no numeric total"
  bps=$(awk '/^  throughput:/ { print $2; exit }' "$TMP/bench.out")
  positive_int "$bps" || die "bench produced no numeric throughput"
  M_KBS=$((bps / 1000))
  positive_int "$M_KBS" || die "bench throughput below 1 KB/s"

  M_RSS=$(measure_rss_kb "$TIQ" build "$WORKLOAD" -o "$TMP/big") ||
    die "cannot measure peak RSS (need /usr/bin/time -l/-v)"
  positive_int "$M_RSS" || die "peak RSS measurement not positive"
}

emit_metrics() {
  echo "binary_size_hello=$M_HELLO"
  echo "binary_size_fib=$M_FIB"
  echo "bench_total_ms=$M_TOTAL"
  echo "bench_throughput_kbs=$M_KBS"
  echo "rss_compile_kb=$M_RSS"
}

base_get() {
  sed -n "s/^$1=//p" "$BASELINE" | head -1
}

# --- record ---------------------------------------------------------------

if [ "$1" = "record" ]; then
  measure
  {
    echo "# tiq perf baseline — regenerate with: make perf-record"
    echo "# os: $(uname -s)"
    emit_metrics
  } >"$BASELINE"
  emit_metrics
  echo "perf: baseline recorded to $BASELINE"
  exit 0
fi

# --- check ----------------------------------------------------------------

[ "$1" = "check" ] || usage

[ -f "$BASELINE" ] || die "baseline not found: $BASELINE (run: make perf-record)"

base_os=$(sed -n 's/^# os: //p' "$BASELINE" | head -1)
[ -n "$base_os" ] || die "baseline corrupt: missing os header"
for key in binary_size_hello binary_size_fib bench_total_ms bench_throughput_kbs rss_compile_kb; do
  val=$(base_get "$key")
  positive_int "$val" || die "baseline corrupt: $key"
done

measure

soft=0
[ "$(uname -s)" = "$base_os" ] || soft=1

if [ "$soft" -eq 0 ]; then
  # Hard gate: binary size is deterministic for a fixed toolchain + OS;
  # allow a +10% band for host toolchain drift, fail on anything bigger.
  for key in binary_size_hello binary_size_fib; do
    want=$(base_get "$key")
    case "$key" in
    binary_size_hello) got=$M_HELLO ;;
    binary_size_fib) got=$M_FIB ;;
    esac
    if [ "$got" -gt $((want * 110 / 100)) ]; then
      die "REGRESSION $key: baseline $want, got $got (+$(((got - want) * 100 / want))%)"
    fi
  done
fi

# Soft reports: machine-dependent numbers, compared for information only.
b_total=$(base_get bench_total_ms)
if [ "$M_TOTAL" -gt $((b_total * 4)) ]; then
  echo "perf: WARNING bench_total_ms $M_TOTAL ms exceeds 4x baseline $b_total ms" >&2
fi
b_rss=$(base_get rss_compile_kb)
if [ "$M_RSS" -gt $((b_rss * 4)) ]; then
  echo "perf: WARNING rss_compile_kb $M_RSS KB exceeds 4x baseline $b_rss KB" >&2
fi

emit_metrics
if [ "$soft" -eq 1 ]; then
  echo "perf: ok (soft check: baseline recorded on $base_os, running $(uname -s))"
else
  echo "perf: ok"
fi
exit 0
