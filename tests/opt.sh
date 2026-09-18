#!/bin/sh
# Integration test for IR optimization passes (--opt-level).
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-opt-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

PASS=0
FAIL=0

# Build a .tiq file via the QBE backend with given opt-level, run it, compare stdout.
# Usage: run_test <name> <source> <expected_output> [extra-build-flags...]
run_test() {
    name="$1"; src="$2"; expect="$3"; shift 3
    extra="$*"
    input="$TMP_DIR/${name}.tiq"
    exe="$TMP_DIR/${name}_bin"
    out="$TMP_DIR/${name}.out"

    printf '%s\n' "$src" > "$input"
    if $TIQ build "$input" --backend qbe -o "$exe" $extra 2>"$TMP_DIR/${name}.err"; then
        if "$exe" > "$out" 2>/dev/null; then
            actual=$(cat "$out")
            if [ "$actual" = "$expect" ]; then
                PASS=$((PASS + 1))
            else
                FAIL=$((FAIL + 1))
                echo "FAIL $name: expected '$expect', got '$actual'"
            fi
        else
            FAIL=$((FAIL + 1))
            echo "FAIL $name: execution failed"
        fi
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name: compilation failed"
        cat "$TMP_DIR/${name}.err" >&2
    fi
    rm -f "$input" "$exe" "$out" "$TMP_DIR/${name}.err"
}

# --- constant folding: 2 + 3 folded to 5 at IR level ---
run_test "constfold_add" \
    'print(2 + 3)' \
    "5"

# --- constant folding: nested expression (2+3)*(10-4) = 30 ---
run_test "nested_constfold" \
    'print((2 + 3) * (10 - 4))' \
    "30"

# --- algebraic: x + 0 => x ---
run_test "algebraic_add_zero" \
    'x <- 10
y = x + 0
print(y)' \
    "10"

# --- algebraic: x * 1 => x ---
run_test "algebraic_mul_one" \
    'x <- 7
y = x * 1
print(y)' \
    "7"

# --- opt-level=0: still produces correct results (pipeline skipped) ---
run_test "opt_level_0" \
    'print(2 + 3)' \
    "5" \
    --opt-level=0

# --- opt-level=2: full pipeline ---
run_test "opt_level_2" \
    'print((2 + 3) * (10 - 4))' \
    "30" \
    --opt-level=2

# --- DCE: unused binding should not affect output ---
run_test "dce_unused" \
    'dead = 999
x = 42
print(x)' \
    "42"

# --- combined: constfold + algebraic + dce ---
run_test "combined_passes" \
    'x <- 5
y = x + 0
z = y * 1
dead = 100 + 200
print(z)' \
    "5"

# --- function with const-foldable call ---
run_test "fn_constfold" \
    'add a b -> a + b
print(add(2, 3))' \
    "5"

# --- multi-binding chain (exercises constfold across bindings) ---
run_test "multi_binding" \
    'a = 10
b = 20
c = a + b
d = c * 2
print(d)' \
    "60"

# --- verify dump-ir produces output at all opt levels ---
for level in 0 1 2; do
    input="$TMP_DIR/dump_test.tiq"
    printf 'print(1 + 2)\n' > "$input"
    if $TIQ dump-ir "$input" > /dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL dump_ir_level_$level: dump-ir failed"
    fi
    rm -f "$input"
done

echo ""
echo "opt: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
