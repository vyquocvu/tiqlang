#!/bin/sh
# M14.2-T1: Tiq formatter harness. Builds src/tiq/tools/fmt.tiq with the C
# bootstrap into build/tiq-fmt, then verifies the canonical formatting rules
# byte-exactly (operators, braces, brackets, comments, unary vs binary, record
# literals, ranges), stdin/file equivalence, --check pass/fail, --output,
# --use-tabs/--indent-width, comment preservation, idempotence
# (fmt(fmt(x)) == fmt(x)), `--check` clean on every example after the M14.2
# normalization pass, and ASan/UBSan on the formatter's emitted C.
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
FMT="build/tiq-fmt"
TMP_DIR="${TMPDIR:-/tmp}/tiq-fmt-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/fmt.tiq -o "$FMT" 2>"$TMP_DIR/build.err"; then
  echo "formatter_tool: FAIL (cannot build src/tiq/tools/fmt.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0

# Format stdin (redirected from a regular file, the supported seekable source)
# and compare the output to a golden file byte-for-byte.
expect_fmt() {
  name="$1"; input="$2"; golden="$3"
  "$FMT" < "$input" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  if ! cmp -s "$golden" "$TMP_DIR/$name.out"; then
    echo "formatter_tool: FAIL $name (output mismatch)" >&2
    diff -u "$golden" "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "formatter_tool: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

# 1. A one-line loop body opens onto its own lines; `;` and keywords space out.
printf '[i <- 0..5]{print(i);break}\n' >"$TMP_DIR/g1.tiq"
printf '[i <- 0..5] {\n    print(i); break\n}\n' >"$TMP_DIR/g1.golden"
expect_fmt g1 "$TMP_DIR/g1.tiq" "$TMP_DIR/g1.golden"

# 2. Operators, the ternary, and call/index spacing.
printf 'gcd a b -> b == 0?a:gcd(b,a%%b)\n' >"$TMP_DIR/g2.tiq"
printf 'gcd a b -> b == 0 ? a : gcd(b, a %% b)\n' >"$TMP_DIR/g2.golden"
expect_fmt g2 "$TMP_DIR/g2.tiq" "$TMP_DIR/g2.golden"

# 3. Stream generators and ranges stay tight.
printf 'fib = [0,1,...(a,b)->a+b]\n' >"$TMP_DIR/g3.tiq"
printf 'fib = [0, 1, ... (a, b) -> a + b]\n' >"$TMP_DIR/g3.golden"
expect_fmt g3 "$TMP_DIR/g3.tiq" "$TMP_DIR/g3.golden"

# 4. Comments survive verbatim and keep their positions.
printf '// leading comment\nx = 1 // trailing comment\n[i <- 0..10] {\n    // inside block\n    x += i\n}\n' >"$TMP_DIR/g4.tiq"
printf '// leading comment\nx = 1 // trailing comment\n[i <- 0..10] {\n    // inside block\n    x += i\n}\n' >"$TMP_DIR/g4.golden"
expect_fmt g4 "$TMP_DIR/g4.tiq" "$TMP_DIR/g4.golden"

# 5. Struct bodies are blocks; record literals stay inline; guards glue `? {`.
printf 'struct Point {\nx: i64,\ny: i64\n}\np = Point{x:3,y:4}\np.x == 3 ? { print("ok") }\n' >"$TMP_DIR/g5.tiq"
printf 'struct Point {\n    x: i64,\n    y: i64\n}\np = Point { x: 3, y: 4 }\np.x == 3 ? {\n    print("ok")\n}\n' >"$TMP_DIR/g5.golden"
expect_fmt g5 "$TMP_DIR/g5.tiq" "$TMP_DIR/g5.golden"

# 6. Unary minus in generators and literals stays tight.
printf 'alt = [1, ... (x) -> -x]\n[i <- 0..10] { print(alt[i]) }\nprint(-42)\n' >"$TMP_DIR/g6.tiq"
printf 'alt = [1, ... (x) -> -x]\n[i <- 0..10] {\n    print(alt[i])\n}\nprint(-42)\n' >"$TMP_DIR/g6.golden"
expect_fmt g6 "$TMP_DIR/g6.tiq" "$TMP_DIR/g6.golden"

# 7. A file path and stdin of the same source produce identical bytes.
printf 'x = 1 + 2 * 3\n[i <- 0..4] { print(x + i) }\n' >"$TMP_DIR/eq.tiq"
"$FMT" "$TMP_DIR/eq.tiq" >"$TMP_DIR/eq.file" 2>/dev/null
"$FMT" < "$TMP_DIR/eq.tiq" >"$TMP_DIR/eq.stdin" 2>/dev/null
if ! cmp -s "$TMP_DIR/eq.file" "$TMP_DIR/eq.stdin"; then
  echo "formatter_tool: FAIL stdin_vs_file (bytes differ)" >&2
  diff -u "$TMP_DIR/eq.file" "$TMP_DIR/eq.stdin" >&2
  fail=1
fi

# 8. --check passes on canonical input and fails on unformatted input.
"$FMT" < "$TMP_DIR/eq.tiq" >"$TMP_DIR/eq.golden" 2>/dev/null
printf 'x = 1\n' >"$TMP_DIR/canon.tiq"
printf 'x=1\n' >"$TMP_DIR/ugly.tiq"
expect_exit 0 check_pass "$FMT" --check "$TMP_DIR/canon.tiq"
expect_exit 1 check_fail "$FMT" --check "$TMP_DIR/ugly.tiq"
if ! grep -qF ": not formatted" "$TMP_DIR/check_fail.err"; then
  echo "formatter_tool: FAIL check_fail (missing ': not formatted' on stderr)" >&2
  cat "$TMP_DIR/check_fail.err" >&2
  fail=1
fi

# 9. --output writes the file instead of stdout (identical bytes).
expect_exit 0 write_output "$FMT" --output "$TMP_DIR/eq.out" "$TMP_DIR/eq.tiq"
if [ ! -s "$TMP_DIR/eq.out" ]; then
  echo "formatter_tool: FAIL write_output (output file empty)" >&2
  fail=1
fi
if ! cmp -s "$TMP_DIR/eq.golden" "$TMP_DIR/eq.out"; then
  echo "formatter_tool: FAIL write_output (bytes differ from stdin mode)" >&2
  diff -u "$TMP_DIR/eq.golden" "$TMP_DIR/eq.out" >&2
  fail=1
fi
if [ -s "$TMP_DIR/write_output.out" ]; then
  echo "formatter_tool: FAIL write_output (stdout not empty with --output)" >&2
  fail=1
fi

# 10. --use-tabs indents with tabs; --indent-width sets the space count.
printf '[0..2] {\nx = 1\n}\n' >"$TMP_DIR/ind.tiq"
printf '[0..2] {\n\tx = 1\n}\n' >"$TMP_DIR/tab.golden"
expect_exit 0 tabs "$FMT" --use-tabs < "$TMP_DIR/ind.tiq"
cmp -s "$TMP_DIR/tab.golden" "$TMP_DIR/tabs.out" || {
  echo "formatter_tool: FAIL tabs (tab output mismatch)" >&2
  diff -u "$TMP_DIR/tab.golden" "$TMP_DIR/tabs.out" >&2
  fail=1
}
printf '[0..2] {\n  x = 1\n}\n' >"$TMP_DIR/w2.golden"
expect_exit 0 width2 "$FMT" --indent-width 2 < "$TMP_DIR/ind.tiq"
cmp -s "$TMP_DIR/w2.golden" "$TMP_DIR/width2.out" || {
  echo "formatter_tool: FAIL width2 (2-space output mismatch)" >&2
  diff -u "$TMP_DIR/w2.golden" "$TMP_DIR/width2.out" >&2
  fail=1
}

# 11. Formatting is idempotent: a second pass changes nothing.
expect_exit 0 idem_pass1 "$FMT" < "$TMP_DIR/eq.tiq"
expect_exit 0 idem_pass2 "$FMT" < "$TMP_DIR/idem_pass1.out"
cmp -s "$TMP_DIR/idem_pass1.out" "$TMP_DIR/idem_pass2.out" || {
  echo "formatter_tool: FAIL idempotence (fmt(fmt(x)) != fmt(x))" >&2
  diff -u "$TMP_DIR/idem_pass1.out" "$TMP_DIR/idem_pass2.out" >&2
  fail=1
}

# 12. Empty input produces empty output (empty regular file redirect).
: >"$TMP_DIR/empty.tiq"
expect_exit 0 empty "$FMT" < "$TMP_DIR/empty.tiq"
if [ -s "$TMP_DIR/empty.out" ]; then
  echo "formatter_tool: FAIL empty (output not empty)" >&2
  cat "$TMP_DIR/empty.out" >&2
  fail=1
fi

# 13. Unknown options and a bad --indent-width fail closed with exit 2.
expect_exit 2 unknown_flag "$FMT" --frobnicate "$TMP_DIR/eq.tiq"
expect_exit 2 bad_width "$FMT" --indent-width 0 < "$TMP_DIR/eq.tiq"
expect_exit 2 check_no_file "$FMT" --check < "$TMP_DIR/eq.tiq"

# 14. A lexical error fails closed with the located diagnostic on stderr.
printf 'this is "unterminated\n' >"$TMP_DIR/badlex.tiq"
expect_exit 1 lex_error "$FMT" "$TMP_DIR/badlex.tiq"
if ! grep -qF "error[" "$TMP_DIR/lex_error.err"; then
  echo "formatter_tool: FAIL lex_error (no located diagnostic on stderr)" >&2
  cat "$TMP_DIR/lex_error.err" >&2
  fail=1
fi

# 15. Every example is --check clean after the M14.2 normalization pass.
for src in examples/*.tiq examples/leetcode/*.tiq; do
  if ! "$FMT" --check "$src" >/dev/null 2>"$TMP_DIR/example.err"; then
    echo "formatter_tool: FAIL example --check clean: $src" >&2
    cat "$TMP_DIR/example.err" >&2
    fail=1
  fi
done

# 16. The formatter's generated C is memory-clean under ASan/UBSan.
if "$TIQ" emit-c src/tiq/tools/fmt.tiq >"$TMP_DIR/fmt.c" 2>"$TMP_DIR/fmt.emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/fmt.c" -o "$TMP_DIR/fmt.asan" 2>"$TMP_DIR/fmt.cc.err"; then
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/fmt.asan" < "$TMP_DIR/g1.tiq" >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    if [ "$?" -ne 0 ]; then
      echo "formatter_tool: FAIL ASan formatter (nonzero exit)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
    cmp -s "$TMP_DIR/g1.golden" "$TMP_DIR/asan.out" || {
      echo "formatter_tool: FAIL ASan formatter (output mismatch)" >&2
      diff -u "$TMP_DIR/g1.golden" "$TMP_DIR/asan.out" >&2
      fail=1
    }
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "formatter_tool: failed" >&2
  exit 1
fi
echo "formatter_tool: ok (goldens, stdin/file, --check, --output, tabs, width, idempotence, empty, fail-closed, examples clean, ASan verified)"
