#!/bin/sh
# M14.1-T1: developer test runner harness. Builds src/tiq/tools/test.tiq with
# the C bootstrap into build/tiq-test, then verifies pass/fail/list/verbose
# behavior against tests/tiq/ (all five fixtures must pass), a deliberately
# failing fixture (must exit 1), a marker-less file (must be skipped), and a
# missing compiler path (must fail closed).
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
RUNNER="build/tiq-test"
TMP_DIR="${TMPDIR:-/tmp}/tiq-test-runner-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/test.tiq -o "$RUNNER" 2>"$TMP_DIR/build.err"; then
  echo "test_runner: FAIL (cannot build src/tiq/tools/test.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$RUNNER" --tiq "$TIQ" "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "test_runner: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

expect_out() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.out"; then
    echo "test_runner: FAIL $name (stdout missing '$pattern')" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_err() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.err"; then
    echo "test_runner: FAIL $name (stderr missing '$pattern')" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

# 1. The repository fixture directory passes as a whole.
expect_exit 0 all tests/tiq
expect_out all "Tests: 5 passed, 0 failed, 0 skipped"

# 2. A single passing fixture reports pass.
expect_exit 0 single tests/tiq/hello_test.tiq
expect_out single "Tests: 1 passed, 0 failed, 0 skipped"

# 3. Verbose mode names the running test.
expect_exit 0 verbose --verbose tests/tiq/hello_test.tiq
expect_out verbose "running test tests/tiq/hello_test.tiq"

# 4. List mode prints each discovered test path and no summary.
expect_exit 0 listed --list tests/tiq
grep -qF "tests/tiq/hello_test.tiq" "$TMP_DIR/listed.out" || {
  echo "test_runner: FAIL listed (hello_test.tiq not listed)" >&2
  cat "$TMP_DIR/listed.out" >&2
  fail=1
}
if grep -qF "Tests:" "$TMP_DIR/listed.out"; then
  echo "test_runner: FAIL listed (summary printed in list mode)" >&2
  fail=1
fi

# 5. A deliberately failing fixture must report failure and exit 1.
printf '//! expected: wrong\nprint("actual")\n' >"$TMP_DIR/fail.tiq"
expect_exit 1 failing --verbose "$TMP_DIR/fail.tiq"
expect_err failing "FAIL: $TMP_DIR/fail.tiq"
expect_err failing "expected: wrong"
expect_err failing "got: actual"
expect_out failing "Tests: 0 passed, 1 failed, 0 skipped"

# 6. A marker-less file is skipped, not run.
printf 'print("silent")\n' >"$TMP_DIR/plain.tiq"
expect_exit 0 skipped "$TMP_DIR/plain.tiq"
expect_out skipped "Tests: 0 passed, 0 failed, 1 skipped"
expect_out skipped "Note: Test files should contain a '//! expected:' marker line"

# 7. A compile error fails the test with the compiler diagnostic surfaced.
printf '//! expected: never\nthis is not tiq\n' >"$TMP_DIR/bad.tiq"
expect_exit 1 compile_error "$TMP_DIR/bad.tiq"
expect_err compile_error "FAIL: $TMP_DIR/bad.tiq"
expect_err compile_error "error[E04]"

# 8. A missing compiler binary fails closed with every test failing.
expect_exit 1 missing_compiler --tiq "$TMP_DIR/no-such-tiq" tests/tiq/hello_test.tiq
expect_err missing_compiler "FAIL: tests/tiq/hello_test.tiq"
expect_out missing_compiler "Tests: 0 passed, 1 failed, 0 skipped"

# 9. No transient build artifacts may remain next to the fixtures.
LEFTOVERS=$(find tests/tiq -name '*.testexe' -o -name '*.testout' -o -name '*.testerr' 2>/dev/null)
if [ -n "$LEFTOVERS" ]; then
  echo "test_runner: FAIL leftover artifacts: $LEFTOVERS" >&2
  fail=1
fi

# 10. The runner's generated C is memory-clean under ASan/UBSan.
if "$TIQ" emit-c src/tiq/tools/test.tiq >"$TMP_DIR/runner.c" 2>"$TMP_DIR/runner.emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/runner.c" -o "$TMP_DIR/runner.asan" 2>"$TMP_DIR/runner.cc.err"; then
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/runner.asan" --tiq "$TIQ" tests/tiq >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    if [ "$?" -ne 0 ]; then
      echo "test_runner: FAIL ASan runner (nonzero exit)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
    grep -qF "Tests: 5 passed, 0 failed, 0 skipped" "$TMP_DIR/asan.out" || {
      echo "test_runner: FAIL ASan runner (summary mismatch)" >&2
      cat "$TMP_DIR/asan.out" >&2
      fail=1
    }
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "test_runner: failed" >&2
  exit 1
fi
echo "test_runner: ok (5 fixtures pass, fail/list/verbose/skip/compile-error/fail-closed/ASan verified)"
