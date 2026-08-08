#!/bin/sh
# Visible Binding Audit — Phase 1: Pin hazards with failing tests.
#
# This script tests each finding from the "Infer properties, never invent names"
# audit. For each hazard, it checks whether the compiler currently rejects the
# problematic pattern (PINNED) or silently accepts it (OPEN).
#
# Exit code: 0 when all findings are PINNED (audit complete);
#            non-zero when any finding is still OPEN.
#
# This script is NOT wired into `make test` — it is a standalone diagnostic
# until Phase 3 resolves the findings.

set -u

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-visible-binding-audit-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if [ ! -x "$TIQ" ]; then
  echo "audit: ERROR: compiler not found at $TIQ (run 'make' first)" >&2
  exit 2
fi

open_count=0
pinned_count=0
passed_count=0

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# expect_rejected <label> <finding> <source> <diagnostic_substring>
#
# The test PASSES (finding PINNED) when the compiler REJECTS the source with
# a diagnostic containing <diagnostic_substring>.
# The test FAILS (finding OPEN) when the compiler accepts the source.
expect_rejected() {
  label="$1"; finding="$2"; source="$3"; diag_pattern="$4"
  input="$TMP_DIR/${label}.tiq"
  err_file="$TMP_DIR/${label}.err"

  printf '%s' "$source" > "$input"
  if "$TIQ" build "$input" -o "$TMP_DIR/${label}.bin" 2>"$err_file"; then
    echo "  OPEN   [$finding] $label — compiler accepted (hazard not yet pinned)"
    open_count=$((open_count + 1))
    return
  fi
  if grep -qF "$diag_pattern" "$err_file"; then
    echo "  PINNED [$finding] $label — rejected with expected diagnostic"
    pinned_count=$((pinned_count + 1))
  else
    echo "  PARTIAL [$finding] $label — rejected but diagnostic mismatch"
    echo "           expected pattern: $diag_pattern"
    echo "           actual stderr:"
    sed 's/^/             /' "$err_file"
    # Still counts as pinned (compiler rejects the hazard), but diagnostic
    # wording may need adjustment in Phase 2/3.
    pinned_count=$((pinned_count + 1))
  fi
}

# expect_accepted_output <label> <finding> <source> <expected_output>
#
# The test PASSES when the compiler accepts the source AND the binary produces
# the expected output. This is for positive tests that should work now and
# continue to work.
expect_accepted_output() {
  label="$1"; finding="$2"; source="$3"; expected="$4"
  input="$TMP_DIR/${label}.tiq"
  err_file="$TMP_DIR/${label}.err"
  out_file="$TMP_DIR/${label}.out"

  printf '%s' "$source" > "$input"
  if ! "$TIQ" build "$input" -o "$TMP_DIR/${label}.bin" 2>"$err_file"; then
    echo "  FAIL   [$finding] $label — compiler rejected (should be accepted)"
    sed 's/^/             /' "$err_file"
    open_count=$((open_count + 1))
    return
  fi
  "$TMP_DIR/${label}.bin" > "$out_file" 2>&1
  actual=$(cat "$out_file")
  if [ "$actual" = "$expected" ]; then
    echo "  PASS   [$finding] $label — output matches"
    passed_count=$((passed_count + 1))
  else
    echo "  FAIL   [$finding] $label — output mismatch"
    echo "           expected: $(echo "$expected" | tr '\n' ' ')"
    echo "           actual:   $(echo "$actual" | tr '\n' ' ')"
    open_count=$((open_count + 1))
  fi
}

# ---------------------------------------------------------------------------
echo "=== Visible Binding Audit ==="
echo ""

# ---------------------------------------------------------------------------
# Finding 1 — Range loops silently inject `i`
# ---------------------------------------------------------------------------
echo "--- Finding 1: Range loops silently inject implicit 'i' ---"

# 1a. Outer `i` is silently shadowed by bare range loop.
expect_rejected \
  "range_implicit_i_shadows_outer" "F1" \
  'i = 100
[0..3] { print(i) }
' \
  "shadows"

# 1b. Nested bare range loops create ambiguous implicit `i`.
expect_rejected \
  "nested_range_ambiguous_i" "F1" \
  '[0..3] {
    [0..3] {
        print(i)
    }
}
' \
  "ambiguous"

echo ""

# ---------------------------------------------------------------------------
# Finding 2 — Stream generators silently inject `x`, `a`, `b`, `i`
# ---------------------------------------------------------------------------
echo "--- Finding 2: Stream generators silently inject magic names ---"

# 2a. Outer `a`/`b` are silently shadowed by 2-seed generator.
expect_rejected \
  "stream_gen_shadows_outer_a_b" "F2" \
  'a = 10
b = 20
fib = [0, 1, ... a + b]
' \
  "shadows"

# 2b. Outer `x` is silently shadowed by 1-seed generator.
expect_rejected \
  "stream_gen_shadows_outer_x" "F2" \
  'x = 99
seq = [1, ... x + 1]
' \
  "shadows"

# 2c. `i` is available inside generator expression without any visible binder.
expect_rejected \
  "stream_gen_index_i_undeclared" "F2" \
  'seq = [1, ... x + i]
' \
  "no visible binding"

echo ""

# ---------------------------------------------------------------------------
# Finding 2 (positive) — Non-commutative 2-seed ordering
# ---------------------------------------------------------------------------
echo "--- Finding 2 (positive): Non-commutative stream ordering ---"

# This positive test must PASS now and continue to pass after implementation.
# It pins the window ordering convention across all backends.
#
# Seeds: [10, 3, ... (a, b) -> a - b]
# Convention: a = seed[1] (newer), b = seed[0] (older)
# gen_expr = a - b = newer - older
#
# seq[0]=10  seq[1]=3  seq[2]=3-10=-7  seq[3]=-7-3=-10
# seq[4]=-10-(-7)=-3  seq[5]=-3-(-10)=7  seq[6]=7-(-3)=10
expect_accepted_output \
  "stream_noncommutative_c" "F2+" \
  'seq = [10, 3, ... (a, b) -> a - b]
[i <- 0..7] { print(seq[i]) }
' \
  "$(printf '10\n3\n-7\n-10\n-3\n7\n10')"

echo ""

# ---------------------------------------------------------------------------
# Finding 3 — `none` is a reserved literal (ALREADY RESOLVED)
# ---------------------------------------------------------------------------
echo "--- Finding 3: 'none' as reserved literal (already resolved) ---"

# 3a. `none` cannot be used as an LHS binding name.
expect_rejected \
  "none_reserved_lhs" "F3" \
  'none = 5
' \
  "expected expression"

# 3b. `none` is not callable (it is a literal, not a function).
expect_rejected \
  "none_not_callable" "F3" \
  'x = none(1)
' \
  "arity mismatch"

echo ""

# ---------------------------------------------------------------------------
# Finding 4 — Builtin call spellings bypass ordinary lexical resolution
# ---------------------------------------------------------------------------
echo "--- Finding 4: Builtin names bypass lexical resolution ---"

# 4a. User binding named `print` should be rejected (reserved-prelude policy).
expect_rejected \
  "builtin_reserved_print" "F4" \
  'print = 5
' \
  "reserved"

# 4b. User binding named `len` should be rejected.
expect_rejected \
  "builtin_reserved_len" "F4" \
  'len = 3
' \
  "reserved"

# 4c. User function named `print` should be rejected.
expect_rejected \
  "builtin_user_fn_print" "F4" \
  'print x -> x + 1
' \
  "reserved"

echo ""

# ---------------------------------------------------------------------------
# Finding 5 — Enum lookup can override same-spelled value binding
# ---------------------------------------------------------------------------
echo "--- Finding 5: Enum/value namespace collision ---"

# 5a. Declaring a value binding with the same name as an enum should be
#     rejected (shared namespace, duplicate name).
expect_rejected \
  "enum_value_namespace_collision" "F5" \
  'enum State { Ready }
State = 1
x = State.Ready
' \
  "duplicate"

echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "=== Summary ==="
total=$((open_count + pinned_count + passed_count))
echo "  Total tests: $total"
echo "  PINNED:      $pinned_count (hazard correctly rejected)"
echo "  PASSED:      $passed_count (positive test correct)"
echo "  OPEN:        $open_count (hazard not yet pinned — implementation needed)"
echo ""

if [ "$open_count" -gt 0 ]; then
  echo "audit: INCOMPLETE — $open_count finding(s) still open"
  echo "       Phase 3 implementation is needed to pin these hazards."
  exit 1
fi

echo "audit: COMPLETE — all findings pinned or passing"
exit 0
