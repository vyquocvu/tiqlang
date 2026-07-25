#!/bin/sh
# Every example must compile and terminate with exit code 0.
# Examples are run as standalone binaries under a watchdog so a
# non-terminating example fails the suite instead of hanging it.
set -u

TMP_DIR="${TMPDIR:-/tmp}/tiq-examples-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

TIMEOUT_SECS=10
fail=0

for src in examples/*.tiq examples/leetcode/*.tiq; do
  name=$(basename "$src" .tiq)
  bin="$TMP_DIR/$name"
  if ! ./build/tiq build "$src" -o "$bin" 2>"$TMP_DIR/$name.err"; then
    echo "examples: FAIL $src (build error)" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
    continue
  fi
  # alarm() survives exec, so SIGALRM lands on the example binary itself.
  perl -e "alarm $TIMEOUT_SECS; exec @ARGV" "$bin" >/dev/null 2>&1
  status=$?
  if [ "$status" -ne 0 ]; then
    if [ "$status" -ge 128 ]; then
      echo "examples: FAIL $src (killed by signal $((status - 128)); did not terminate within ${TIMEOUT_SECS}s?)" >&2
    else
      echo "examples: FAIL $src (exit $status)" >&2
    fi
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "examples: failed" >&2
  exit 1
fi
echo "examples: ok"
