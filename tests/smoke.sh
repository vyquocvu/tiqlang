#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-smoke-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

./build/tiq --version
./build/tiq emit-c examples/hello.tiq > "$TMP_DIR/hello.c"
./build/tiq build examples/hello.tiq -o "$TMP_DIR/hello"

# Runtime helpers must use int64_t in an i64 world (plan 1.3)
printf 'fs_write("p", "d")\nx = json_parse_int("42")\nprint(x)\n' > "$TMP_DIR/rt_i64.tiq"
./build/tiq emit-c "$TMP_DIR/rt_i64.tiq" > "$TMP_DIR/rt_i64.c"
if ! grep -q 'static int64_t tiq_fs_write' "$TMP_DIR/rt_i64.c"; then
  echo "tiq_fs_write does not return int64_t" >&2
  exit 1
fi
if grep -qE 'static int tiq_' "$TMP_DIR/rt_i64.c"; then
  echo "runtime helper still returns narrow int:" >&2
  grep -E 'static int tiq_' "$TMP_DIR/rt_i64.c" >&2
  exit 1
fi

# json_parse_int must not truncate 64-bit values
printf 'big = json_parse_int("5000000000")\nprint(big)\n' > "$TMP_DIR/rt_i64_parse.tiq"
./build/tiq build "$TMP_DIR/rt_i64_parse.tiq" -o "$TMP_DIR/rt_i64_parse"
[ "$("$TMP_DIR/rt_i64_parse")" = "5000000000" ]

# Just verify compilation succeeds
[ -x "$TMP_DIR/hello" ]

SPECIAL_OUTPUT="$TMP_DIR/hello path;quote\"name"
./build/tiq build examples/hello.tiq -o "$SPECIAL_OUTPUT"
[ -x "$SPECIAL_OUTPUT" ]

SPECIAL_TMPDIR="$TMP_DIR/tmp path;quote\"dir"
mkdir -p "$SPECIAL_TMPDIR"
TMPDIR="$SPECIAL_TMPDIR" ./build/tiq build examples/hello.tiq -o "$TMP_DIR/tmpdir-hello"
[ -x "$TMP_DIR/tmpdir-hello" ]
if find "$SPECIAL_TMPDIR" -mindepth 1 -print | grep . >/dev/null; then
  echo "temporary C file was not removed" >&2
  exit 1
fi

if env CC="$TMP_DIR/missing-cc" ./build/tiq build examples/hello.tiq -o "$TMP_DIR/no-compiler" 2>"$TMP_DIR/missing-cc.err"; then
  echo "expected missing host C compiler to fail" >&2
  exit 1
fi
if [ -e "$TMP_DIR/no-compiler" ]; then
  echo "missing host C compiler unexpectedly produced an executable" >&2
  exit 1
fi
grep 'cannot execute host C compiler' "$TMP_DIR/missing-cc.err" >/dev/null

printf 'x <- 0\n[0..5] { x += 1 }\n' > "$TMP_DIR/bracket_loop_count.tiq"
./build/tiq build "$TMP_DIR/bracket_loop_count.tiq" -o "$TMP_DIR/bracket_loop_count" 2>"$TMP_DIR/bracket_loop_count.err"
[ -x "$TMP_DIR/bracket_loop_count" ]

printf 'total <- 0\n[0..5] { total += i }\n' > "$TMP_DIR/bracket_sum.tiq"
./build/tiq build "$TMP_DIR/bracket_sum.tiq" -o "$TMP_DIR/bracket_sum" 2>"$TMP_DIR/bracket_sum.err"
[ -x "$TMP_DIR/bracket_sum" ]

printf '[0..3] { i }\n' > "$TMP_DIR/bracket_loop.tiq"
./build/tiq build "$TMP_DIR/bracket_loop.tiq" -o "$TMP_DIR/bracket_loop" 2>"$TMP_DIR/bracket_loop.err"
[ -x "$TMP_DIR/bracket_loop" ]

printf '[0..3] { i; break }\n' > "$TMP_DIR/bracket_break.tiq"
./build/tiq build "$TMP_DIR/bracket_break.tiq" -o "$TMP_DIR/bracket_break" 2>"$TMP_DIR/bracket_break.err"
[ -x "$TMP_DIR/bracket_break" ]

printf 'x <- 0\n[0..3] { x += 1; skip; x += 100 }\n' > "$TMP_DIR/bracket_skip.tiq"
./build/tiq build "$TMP_DIR/bracket_skip.tiq" -o "$TMP_DIR/bracket_skip" 2>"$TMP_DIR/bracket_skip.err"
[ -x "$TMP_DIR/bracket_skip" ]

printf 'x <- 0\n[x < 5] { x += 1 }\n' > "$TMP_DIR/while_loop.tiq"
./build/tiq build "$TMP_DIR/while_loop.tiq" -o "$TMP_DIR/while_loop" 2>"$TMP_DIR/while_loop.err"
[ -x "$TMP_DIR/while_loop" ]

printf 'xs <- [1, 2, 3]\n' > "$TMP_DIR/array_literal.tiq"
./build/tiq build "$TMP_DIR/array_literal.tiq" -o "$TMP_DIR/array_literal" 2>"$TMP_DIR/array_literal.err"
[ -x "$TMP_DIR/array_literal" ]

printf 'xs <- [1, 2, 3]\nxs[0] <- 99\n' > "$TMP_DIR/array_assign.tiq"
./build/tiq build "$TMP_DIR/array_assign.tiq" -o "$TMP_DIR/array_assign" 2>"$TMP_DIR/array_assign.err"
[ -x "$TMP_DIR/array_assign" ]

printf 'xs <- [10, 20, 30]\nlen(xs)\n' > "$TMP_DIR/array_len.tiq"
./build/tiq build "$TMP_DIR/array_len.tiq" -o "$TMP_DIR/array_len" 2>"$TMP_DIR/array_len.err"
[ -x "$TMP_DIR/array_len" ]

printf 's <- "Hello World"\nsub = s[0..5]\nlen(sub)\n' > "$TMP_DIR/str_slice.tiq"
./build/tiq build "$TMP_DIR/str_slice.tiq" -o "$TMP_DIR/str_slice" 2>"$TMP_DIR/str_slice.err"
[ -x "$TMP_DIR/str_slice" ]

printf 'fib = [0, 1, ... a + b]\nfib[10]\n' > "$TMP_DIR/stream_index.tiq"
./build/tiq build "$TMP_DIR/stream_index.tiq" -o "$TMP_DIR/stream_index" 2>"$TMP_DIR/stream_index.err"
[ -x "$TMP_DIR/stream_index" ]

printf 'fact = [1, ... i * x]\nfact[5]\n' > "$TMP_DIR/stream_single_seed.tiq"
./build/tiq build "$TMP_DIR/stream_single_seed.tiq" -o "$TMP_DIR/stream_single_seed" 2>"$TMP_DIR/stream_single_seed.err"
[ -x "$TMP_DIR/stream_single_seed" ]

printf 'fib = [0, 1, ... a + b]\nfib[0]\nfib[1]\nfib[2]\nfib[3]\nfib[4]\nfib[5]\nfib[6]\nfib[7]\nfib[8]\nfib[9]\nfib[10]\n' > "$TMP_DIR/fib_step_by_step.tiq"
./build/tiq build "$TMP_DIR/fib_step_by_step.tiq" -o "$TMP_DIR/fib_step_by_step" 2>"$TMP_DIR/fib_step_by_step.err"
[ -x "$TMP_DIR/fib_step_by_step" ]

printf 'fib = [0, 1, ... a + b]\nfib[0]\nfib[1]\nfib[2]\nfib[3]\nfib[4]\nfib[5]\n' > "$TMP_DIR/fib_explain.tiq"
./build/tiq build "$TMP_DIR/fib_explain.tiq" -o "$TMP_DIR/fib_explain" 2>"$TMP_DIR/fib_explain.err"
[ -x "$TMP_DIR/fib_explain" ]

printf 'fib = [0, 1, ... a + b]\nfib[10]\n' > "$TMP_DIR/fib_verify.tiq"
./build/tiq build "$TMP_DIR/fib_verify.tiq" -o "$TMP_DIR/fib_verify" 2>"$TMP_DIR/fib_verify.err"
[ -x "$TMP_DIR/fib_verify" ]

printf 'pow b -> [1, ... x * b]\npow(2)[0]\npow(2)[10]\npow(3)[3]\n' > "$TMP_DIR/stream_func.tiq"
./build/tiq build "$TMP_DIR/stream_func.tiq" -o "$TMP_DIR/stream_func" 2>"$TMP_DIR/stream_func.err"
[ -x "$TMP_DIR/stream_func" ]

printf 'evens = [0, ... x + 2]\n[0..5] { evens[i] }\n' > "$TMP_DIR/stream_bracket_loop.tiq"
./build/tiq build "$TMP_DIR/stream_bracket_loop.tiq" -o "$TMP_DIR/stream_bracket_loop" 2>"$TMP_DIR/stream_bracket_loop.err"
[ -x "$TMP_DIR/stream_bracket_loop" ]

printf 'x <- [1, 2, 3]\ny <- move x\n' > "$TMP_DIR/move_basic.tiq"
./build/tiq build "$TMP_DIR/move_basic.tiq" -o "$TMP_DIR/move_basic" 2>"$TMP_DIR/move_basic.err"
[ -x "$TMP_DIR/move_basic" ]

printf 'x <- 0\ny <- move x\nx += 42\n' > "$TMP_DIR/move_reassign.tiq"
./build/tiq build "$TMP_DIR/move_reassign.tiq" -o "$TMP_DIR/move_reassign" 2>"$TMP_DIR/move_reassign.err"
[ -x "$TMP_DIR/move_reassign" ]

printf 'x <- 42\ny <- move x + 1\n' > "$TMP_DIR/move_compound.tiq"
./build/tiq build "$TMP_DIR/move_compound.tiq" -o "$TMP_DIR/move_compound" 2>"$TMP_DIR/move_compound.err"
[ -x "$TMP_DIR/move_compound" ]

printf '{\ndefer 2\n1\n}\n' > "$TMP_DIR/defer_basic.tiq"
./build/tiq build "$TMP_DIR/defer_basic.tiq" -o "$TMP_DIR/defer_basic" 2>"$TMP_DIR/defer_basic.err"
[ -x "$TMP_DIR/defer_basic" ]

printf '{\ndefer 2\ndefer 3\n4\n}\n' > "$TMP_DIR/defer_reverse.tiq"
./build/tiq build "$TMP_DIR/defer_reverse.tiq" -o "$TMP_DIR/defer_reverse" 2>"$TMP_DIR/defer_reverse.err"
[ -x "$TMP_DIR/defer_reverse" ]

printf 'x <- 0\n{\ndefer 99\nx\n}\n' > "$TMP_DIR/defer_with_scope.tiq"
./build/tiq build "$TMP_DIR/defer_with_scope.tiq" -o "$TMP_DIR/defer_with_scope" 2>"$TMP_DIR/defer_with_scope.err"
[ -x "$TMP_DIR/defer_with_scope" ]

printf 'p = "%s/m6_test.txt"\nfs_write(p, "hello m6")\ne = fs_exists(p)\nr = fs_read(p)\nlen(r)\n' "$TMP_DIR" > "$TMP_DIR/m6_fs.tiq"
./build/tiq build "$TMP_DIR/m6_fs.tiq" -o "$TMP_DIR/m6_fs" 2>"$TMP_DIR/m6_fs.err"
[ -x "$TMP_DIR/m6_fs" ]
"$TMP_DIR/m6_fs"

printf 'cmd = "true"\nres = proc_exec(cmd)\nval = json_parse_int("42")\nenc = json_encode_str("test")\nnet = net_fetch("http://localhost")\n' > "$TMP_DIR/m6_sys.tiq"
./build/tiq build "$TMP_DIR/m6_sys.tiq" -o "$TMP_DIR/m6_sys" 2>"$TMP_DIR/m6_sys.err"
[ -x "$TMP_DIR/m6_sys" ]
"$TMP_DIR/m6_sys"

printf 'arr = [0; 5]\ns = "hello"\n' > "$TMP_DIR/m7_features.tiq"
./build/tiq build "$TMP_DIR/m7_features.tiq" -o "$TMP_DIR/m7_features" 2>"$TMP_DIR/m7_features.err"
[ -x "$TMP_DIR/m7_features" ]
"$TMP_DIR/m7_features"

# spawn/chan have no runtime yet: builds must fail closed (plan 1.2)
printf 'sp = spawn 10\n' > "$TMP_DIR/m7_spawn.tiq"
if ./build/tiq build "$TMP_DIR/m7_spawn.tiq" -o "$TMP_DIR/m7_spawn" 2>"$TMP_DIR/m7_spawn.err"; then
    echo "spawn unexpectedly compiled" >&2
    exit 1
fi
[ ! -e "$TMP_DIR/m7_spawn" ]
printf 'ch = chan int\n' > "$TMP_DIR/m7_chan.tiq"
if ./build/tiq build "$TMP_DIR/m7_chan.tiq" -o "$TMP_DIR/m7_chan" 2>"$TMP_DIR/m7_chan.err"; then
    echo "chan unexpectedly compiled" >&2
    exit 1
fi
[ ! -e "$TMP_DIR/m7_chan" ]

printf 'x = 10\nres = match x { 10 => 100, 20 => 200 }\n' > "$TMP_DIR/m8_match.tiq"
./build/tiq build "$TMP_DIR/m8_match.tiq" -o "$TMP_DIR/m8_match" 2>"$TMP_DIR/m8_match.err"
[ -x "$TMP_DIR/m8_match" ]
"$TMP_DIR/m8_match"

# M9 borrow checking does not exist yet: borrows must fail closed instead
# of silently emitting a value copy (DOC_REVIEW D).
printf 'x <- 42\nb = &x\n' > "$TMP_DIR/m9_borrow.tiq"
if ./build/tiq build "$TMP_DIR/m9_borrow.tiq" -o "$TMP_DIR/m9_borrow" 2>"$TMP_DIR/m9_borrow.err"; then
    echo "borrow unexpectedly compiled" >&2
    exit 1
fi
[ ! -e "$TMP_DIR/m9_borrow" ]

# '!' is logical negation only; print is the print builtin (LANGUAGE_SPEC §12)
printf 'flag = !false\nprint(flag)\nprint(!flag ? 10 : 20)\n' > "$TMP_DIR/not_bool.tiq"
./build/tiq build "$TMP_DIR/not_bool.tiq" -o "$TMP_DIR/not_bool" 2>"$TMP_DIR/not_bool.err"
[ -x "$TMP_DIR/not_bool" ]
"$TMP_DIR/not_bool" > "$TMP_DIR/not_bool.out"
printf 'true\n20\n' > "$TMP_DIR/not_bool.expected"
if ! cmp -s "$TMP_DIR/not_bool.expected" "$TMP_DIR/not_bool.out"; then
  echo "unary not output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/not_bool.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/not_bool.out" >&2
  exit 1
fi

# print builtin formats each type deterministically (LANGUAGE_SPEC §12)
printf 'print(7)\nprint("hi")\nprint(3.5)\nprint(1 < 2)\ns = "Hello World"\nprint(s[0..5])\n' > "$TMP_DIR/print_builtin.tiq"
./build/tiq build "$TMP_DIR/print_builtin.tiq" -o "$TMP_DIR/print_builtin" 2>"$TMP_DIR/print_builtin.err"
[ -x "$TMP_DIR/print_builtin" ]
"$TMP_DIR/print_builtin" > "$TMP_DIR/print_builtin.out"
printf '7\nhi\n3.5\ntrue\nHello\n' > "$TMP_DIR/print_builtin.expected"
if ! cmp -s "$TMP_DIR/print_builtin.expected" "$TMP_DIR/print_builtin.out"; then
  echo "print builtin output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/print_builtin.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/print_builtin.out" >&2
  exit 1
fi

# Loop headers take a full expression: '&&' and '||' parse without
# extra parentheses now that '|' is no longer the body separator.
printf 'a <- 0\n[a < 3 && a >= 0] { a += 1 }\nprint(a)\nb <- 0\n[b < 2 || false] { b += 1 }\nprint(b)\n' > "$TMP_DIR/loop_domain_expr.tiq"
./build/tiq build "$TMP_DIR/loop_domain_expr.tiq" -o "$TMP_DIR/loop_domain_expr" 2>"$TMP_DIR/loop_domain_expr.err"
[ -x "$TMP_DIR/loop_domain_expr" ]
"$TMP_DIR/loop_domain_expr" > "$TMP_DIR/loop_domain_expr.out"
printf '3\n2\n' > "$TMP_DIR/loop_domain_expr.expected"
if ! cmp -s "$TMP_DIR/loop_domain_expr.expected" "$TMP_DIR/loop_domain_expr.out"; then
  echo "loop domain expression output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/loop_domain_expr.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/loop_domain_expr.out" >&2
  exit 1
fi

# M12.2: integer values are 64-bit (i64 default, LANGUAGE_SPEC §11)
printf 'x = 5000000000\nprint(x)\nprint(2147483647 + 1)\ny <- 1000000000\ny *= 5\nprint(y)\n' > "$TMP_DIR/i64_values.tiq"
./build/tiq build "$TMP_DIR/i64_values.tiq" -o "$TMP_DIR/i64_values" 2>"$TMP_DIR/i64_values.err"
[ -x "$TMP_DIR/i64_values" ]
"$TMP_DIR/i64_values" > "$TMP_DIR/i64_values.out"
printf '5000000000\n2147483648\n5000000000\n' > "$TMP_DIR/i64_values.expected"
if ! cmp -s "$TMP_DIR/i64_values.expected" "$TMP_DIR/i64_values.out"; then
  echo "i64 value output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/i64_values.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/i64_values.out" >&2
  exit 1
fi

echo "smoke: ok"
