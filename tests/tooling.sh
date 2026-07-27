#!/bin/sh
# Run all M5 tooling tests

set -eu

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

if [ ! -f "./build/tiq" ]; then
    echo "Building tiq first..."
    make
fi

export TIQ="$PROJECT_DIR/build/tiq"

echo ""
echo "========================================"
echo "M5 Tooling Test Suite"
echo "========================================"
echo ""

FAILED=0

run_test() {
    TEST_NAME="$1"
    shift
    echo "--- $TEST_NAME ---"
    if "$@"; then
        echo "PASS: $TEST_NAME"
    else
        echo "FAIL: $TEST_NAME"
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

# Run all tooling tests
run_test "formatter" sh tests/tooling/formatter.sh
run_test "check" sh tests/tooling/check.sh
run_test "test runner" sh tests/tooling/test_runner.sh
run_test "run" sh tests/tooling/run.sh
run_test "benchmark" sh tests/tooling/benchmark.sh
run_test "init and cache" sh tests/tooling/init_cache.sh
run_test "lsp" sh tests/tooling/lsp.sh

echo ""
echo "========================================"
if [ $FAILED -eq 0 ]; then
    echo "All tooling tests passed!"
else
    echo "FAILED: $FAILED test(s)"
fi
echo "========================================"

exit $FAILED
