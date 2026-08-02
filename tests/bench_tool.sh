#!/bin/sh
# M14.3-T1: compiler phase benchmark harness. Builds src/tiq/tools/bench.tiq
# with the C bootstrap into build/tiq-bench, then verifies the stable output
# shape (per-file header, per-phase lines, throughput, summary), `-i`
# iteration parsing including fail-closed guards, single-file vs directory
# targets (dotfiles and non-.tiq names skipped), exit codes for no files / a
# missing file, and ASan/UBSan on the benchmark's emitted C.
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
BENCH="build/tiq-bench"
TMP_DIR="${TMPDIR:-/tmp}/tiq-bench-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/bench.tiq -o "$BENCH" 2>"$TMP_DIR/build.err"; then
  echo "bench_tool: FAIL (cannot build src/tiq/tools/bench.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "bench_tool: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

expect_out() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.out"; then
    echo "bench_tool: FAIL $name (stdout missing '$pattern')" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_err() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.err"; then
    echo "bench_tool: FAIL $name (stderr missing '$pattern')" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

# 1. A single file at default -i 1 has the stable section/per-phase shape.
expect_exit 0 single "$BENCH" examples/gcd.tiq
expect_out single "=== Benchmark Results ==="
expect_out single "examples/gcd.tiq ("
expect_out single "bytes)"
expect_out single "  lexer:"
expect_out single "  parser:"
expect_out single "  semantic:"
expect_out single "  total:"
expect_out single "  throughput:"
expect_out single "=== Summary ==="
expect_out single "Files:       1"
expect_out single "Iterations:  1"

# 2. -i parses and is reflected in the summary; timing lines stay numeric.
expect_exit 0 many "$BENCH" -i 50 examples/gcd.tiq
expect_out many "Iterations:  50"
if ! grep -qE "^  (lexer|parser|semantic|total): *[0-9]+$" "$TMP_DIR/many.out"; then
  echo "bench_tool: FAIL many (non-numeric phase line)" >&2
  cat "$TMP_DIR/many.out" >&2
  fail=1
fi

# 3. A directory target scans sorted .tiq files, skipping dotfiles and others.
mkdir -p "$TMP_DIR/dir"
printf 'a <- 1\nprint(a)\n' >"$TMP_DIR/dir/b.tiq"
printf 'x = 2\nprint(x)\n' >"$TMP_DIR/dir/a.tiq"
printf 'hidden = 3\n' >"$TMP_DIR/dir/.hidden.tiq"
printf 'not tiq\n' >"$TMP_DIR/dir/notes.txt"
expect_exit 0 dirscan "$BENCH" -i 3 "$TMP_DIR/dir"
expect_out dirscan "$TMP_DIR/dir/a.tiq"
expect_out dirscan "$TMP_DIR/dir/b.tiq"
expect_out dirscan "Files:       2"
if grep -qF ".hidden.tiq" "$TMP_DIR/dirscan.out"; then
  echo "bench_tool: FAIL dirscan (dotfile benchmarked)" >&2
  cat "$TMP_DIR/dirscan.out" >&2
  fail=1
fi
# a.tiq must precede b.tiq (fs_list sorts, a < b).
a_line=$(grep -nF "$TMP_DIR/dir/a.tiq" "$TMP_DIR/dirscan.out" | head -1 | cut -d: -f1)
b_line=$(grep -nF "$TMP_DIR/dir/b.tiq" "$TMP_DIR/dirscan.out" | head -1 | cut -d: -f1)
if [ -z "$a_line" ] || [ -z "$b_line" ] || [ "$a_line" -ge "$b_line" ]; then
  echo "bench_tool: FAIL dirscan (files not in sorted order)" >&2
  cat "$TMP_DIR/dirscan.out" >&2
  fail=1
fi

# 4. No targets, a bad -i, and a non-.tiq directory all fail closed with exit 2.
expect_exit 2 noargs "$BENCH"
expect_err noargs "tiq bench: no .tiq files given"
expect_exit 2 bad_i "$BENCH" -i abc examples/gcd.tiq
expect_err bad_i "tiq bench: -i iterations must be a positive integer"
expect_exit 2 bad_i0 "$BENCH" -i 0 examples/gcd.tiq
expect_err bad_i0 "tiq bench: -i iterations must be a positive integer"
expect_exit 2 empty_dir "$BENCH" "$TMP_DIR"
expect_err empty_dir "tiq bench: no .tiq files given"

# 5. A missing file fails closed with exit 1 and the located diagnostic.
expect_exit 1 missing "$BENCH" "$TMP_DIR/no-such-file.tiq"
expect_err missing "tiq bench: cannot read $TMP_DIR/no-such-file.tiq"

# 6. Unknown flags are ignored as non-targets (target heuristic) only when
# another target is present; alone they yield no files -> exit 2.
expect_exit 2 only_flag "$BENCH" --frobnicate

# 7. The benchmark's generated C is memory-clean under ASan/UBSan.
if "$TIQ" emit-c src/tiq/tools/bench.tiq >"$TMP_DIR/bench.c" 2>"$TMP_DIR/bench.emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/bench.c" -o "$TMP_DIR/bench.asan" 2>"$TMP_DIR/bench.cc.err"; then
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/bench.asan" -i 5 examples/gcd.tiq >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    if [ "$?" -ne 0 ]; then
      echo "bench_tool: FAIL ASan bench (nonzero exit)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
    grep -qF "=== Benchmark Results ===" "$TMP_DIR/asan.out" || {
      echo "bench_tool: FAIL ASan bench (missing results header)" >&2
      cat "$TMP_DIR/asan.out" >&2
      fail=1
    }
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "bench_tool: failed" >&2
  exit 1
fi
echo "bench_tool: ok (shape, -i parsing, dir scan order, fail-closed exits, ASan verified)"
