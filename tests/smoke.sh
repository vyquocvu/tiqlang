#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-smoke-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

./build/tiq --version
./build/tiq emit-c examples/hello.tiq > "$TMP_DIR/hello.c"
./build/tiq build examples/hello.tiq -o "$TMP_DIR/hello"

# Runtime helpers must use int64_t in an i64 world (plan 1.3)
printf 'import "std/json.tiq"\nfs_write("p", "d")\nx = json_parse_int("42")\nprint(x)\n' > "$TMP_DIR/rt_i64.tiq"
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
printf 'import "std/json.tiq"\nbig = json_parse_int("5000000000")\nprint(big)\n' > "$TMP_DIR/rt_i64_parse.tiq"
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
# Exclude Apple's xcrun compiler-launcher cache directory (`xcrun_db`) and
# any other non-Tiq platform artifacts that the host C toolchain may create
# in TMPDIR; the test only cares about files that the Tiq build path leaks.
if find "$SPECIAL_TMPDIR" -mindepth 1 -print 2>/dev/null \
     | grep -v -E '/(xcrun_db|org\.llvm\.clang)(\.dir)?(/.*)?$' \
     | grep . >/dev/null; then
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

printf 'x <- 0\n[i <- 0..5] { x += 1 }\n' > "$TMP_DIR/bracket_loop_count.tiq"
./build/tiq build "$TMP_DIR/bracket_loop_count.tiq" -o "$TMP_DIR/bracket_loop_count" 2>"$TMP_DIR/bracket_loop_count.err"
[ -x "$TMP_DIR/bracket_loop_count" ]

printf 'total <- 0\n[i <- 0..5] { total += i }\n' > "$TMP_DIR/bracket_sum.tiq"
./build/tiq build "$TMP_DIR/bracket_sum.tiq" -o "$TMP_DIR/bracket_sum" 2>"$TMP_DIR/bracket_sum.err"
[ -x "$TMP_DIR/bracket_sum" ]

printf '[i <- 0..3] { i }\n' > "$TMP_DIR/bracket_loop.tiq"
./build/tiq build "$TMP_DIR/bracket_loop.tiq" -o "$TMP_DIR/bracket_loop" 2>"$TMP_DIR/bracket_loop.err"
[ -x "$TMP_DIR/bracket_loop" ]

printf '[i <- 0..3] { i; break }\n' > "$TMP_DIR/bracket_break.tiq"
./build/tiq build "$TMP_DIR/bracket_break.tiq" -o "$TMP_DIR/bracket_break" 2>"$TMP_DIR/bracket_break.err"
[ -x "$TMP_DIR/bracket_break" ]

printf 'x <- 0\n[i <- 0..3] { x += 1; skip; x += 100 }\n' > "$TMP_DIR/bracket_skip.tiq"
./build/tiq build "$TMP_DIR/bracket_skip.tiq" -o "$TMP_DIR/bracket_skip" 2>"$TMP_DIR/bracket_skip.err"
[ -x "$TMP_DIR/bracket_skip" ]

printf 'x <- 0\n[x < 5] { x += 1 }\n' > "$TMP_DIR/while_loop.tiq"
./build/tiq build "$TMP_DIR/while_loop.tiq" -o "$TMP_DIR/while_loop" 2>"$TMP_DIR/while_loop.err"
[ -x "$TMP_DIR/while_loop" ]

# M25: one-arm conditional (block form) replaces the legacy `?[cond]`.
printf 'x <- 0\n1 == 1 ? { x <- 42 }\nprint(x)\n' > "$TMP_DIR/bracket_cond.tiq"
./build/tiq build "$TMP_DIR/bracket_cond.tiq" -o "$TMP_DIR/bracket_cond"
[ "$("$TMP_DIR/bracket_cond")" = "42" ]

printf 'x <- 10\nx > 5 ? { print("big") }\n' > "$TMP_DIR/bracket_cond_oneline.tiq"
./build/tiq build "$TMP_DIR/bracket_cond_oneline.tiq" -o "$TMP_DIR/bracket_cond_oneline"
[ "$("$TMP_DIR/bracket_cond_oneline")" = "big" ]

printf '[i <- 0..10] { i == 5 ? { break }; print(i) }\n' > "$TMP_DIR/bracket_cond_break.tiq"
./build/tiq build "$TMP_DIR/bracket_cond_break.tiq" -o "$TMP_DIR/bracket_cond_break"
[ "$("$TMP_DIR/bracket_cond_break")" = "0
1
2
3
4" ]

printf 'true ? { break }\n' > "$TMP_DIR/bracket_cond_break_err.tiq"
if ./build/tiq build "$TMP_DIR/bracket_cond_break_err.tiq" -o "$TMP_DIR/bracket_cond_break_err" 2>"$TMP_DIR/bracket_cond_break_err.err"; then
  echo "expected break outside loop to fail" >&2
  exit 1
fi
grep -q 'break outside loop' "$TMP_DIR/bracket_cond_break_err.err"

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

printf 'fib = [0, 1, ... (a, b) -> a + b]\nfib[10]\n' > "$TMP_DIR/stream_index.tiq"
./build/tiq build "$TMP_DIR/stream_index.tiq" -o "$TMP_DIR/stream_index" 2>"$TMP_DIR/stream_index.err"
[ -x "$TMP_DIR/stream_index" ]

printf 'fact = [1, ... (x; i) -> i * x]\nfact[5]\n' > "$TMP_DIR/stream_single_seed.tiq"
./build/tiq build "$TMP_DIR/stream_single_seed.tiq" -o "$TMP_DIR/stream_single_seed" 2>"$TMP_DIR/stream_single_seed.err"
[ -x "$TMP_DIR/stream_single_seed" ]

printf 'fib = [0, 1, ... (a, b) -> a + b]\nfib[0]\nfib[1]\nfib[2]\nfib[3]\nfib[4]\nfib[5]\nfib[6]\nfib[7]\nfib[8]\nfib[9]\nfib[10]\n' > "$TMP_DIR/fib_step_by_step.tiq"
./build/tiq build "$TMP_DIR/fib_step_by_step.tiq" -o "$TMP_DIR/fib_step_by_step" 2>"$TMP_DIR/fib_step_by_step.err"
[ -x "$TMP_DIR/fib_step_by_step" ]

printf 'fib = [0, 1, ... (a, b) -> a + b]\nfib[0]\nfib[1]\nfib[2]\nfib[3]\nfib[4]\nfib[5]\n' > "$TMP_DIR/fib_explain.tiq"
./build/tiq build "$TMP_DIR/fib_explain.tiq" -o "$TMP_DIR/fib_explain" 2>"$TMP_DIR/fib_explain.err"
[ -x "$TMP_DIR/fib_explain" ]

printf 'fib = [0, 1, ... (a, b) -> a + b]\nfib[10]\n' > "$TMP_DIR/fib_verify.tiq"
./build/tiq build "$TMP_DIR/fib_verify.tiq" -o "$TMP_DIR/fib_verify" 2>"$TMP_DIR/fib_verify.err"
[ -x "$TMP_DIR/fib_verify" ]

printf 'pow b -> [1, ... (x) -> x * b]\npow(2)[0]\npow(2)[10]\npow(3)[3]\n' > "$TMP_DIR/stream_func.tiq"
./build/tiq build "$TMP_DIR/stream_func.tiq" -o "$TMP_DIR/stream_func" 2>"$TMP_DIR/stream_func.err"
[ -x "$TMP_DIR/stream_func" ]

printf 'evens = [0, ... (x) -> x + 2]\n[i <- 0..5] { evens[i] }\n' > "$TMP_DIR/stream_bracket_loop.tiq"
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

# net_fetch("ftp://...") fails the scheme check without touching the network
# (LANGUAGE_SPEC §19.2), keeping this test deterministic.
printf 'import "std/json.tiq"\nimport "std/net.tiq"\ncmd = "true"\nres = proc_exec(cmd)\nval = json_parse_int("42")\nenc = json_encode_str("test")\nnet = net_fetch("ftp://unused")\n' > "$TMP_DIR/m6_sys.tiq"
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

# M12.7.2: match requires wildcard arm
printf 'x = 10\nres = match x { 10 => 100, _ => 0 }\nprint(res)\n' > "$TMP_DIR/m8_match.tiq"
./build/tiq build "$TMP_DIR/m8_match.tiq" -o "$TMP_DIR/m8_match" 2>"$TMP_DIR/m8_match.err"
[ -x "$TMP_DIR/m8_match" ]
[ "$("$TMP_DIR/m8_match")" = "100" ]

# M9.1: borrows outside call arguments still fail closed (no stored borrows).
printf 'x <- 42\nb = &x\n' > "$TMP_DIR/m9_borrow.tiq"
if ./build/tiq build "$TMP_DIR/m9_borrow.tiq" -o "$TMP_DIR/m9_borrow" 2>"$TMP_DIR/m9_borrow.err"; then
    echo "borrow unexpectedly compiled" >&2
    exit 1
fi
[ ! -e "$TMP_DIR/m9_borrow" ]

# M9.1: &mut parameters mutate the caller's binding; & parameters read it.
printf 'bump r:&mut i64 -> {\n    r <- r + 10\n}\nshow v:&i64 -> print(v)\nn <- 32\nbump(&mut n)\nshow(&n)\nprint(n)\n' > "$TMP_DIR/m9_borrow_params.tiq"
./build/tiq build "$TMP_DIR/m9_borrow_params.tiq" -o "$TMP_DIR/m9_borrow_params" 2>"$TMP_DIR/m9_borrow_params.err"
[ -x "$TMP_DIR/m9_borrow_params" ]
"$TMP_DIR/m9_borrow_params" > "$TMP_DIR/m9_borrow_params.out"
printf '42\n42\n' > "$TMP_DIR/m9_borrow_params.expected"
if ! cmp -s "$TMP_DIR/m9_borrow_params.expected" "$TMP_DIR/m9_borrow_params.out"; then
    echo "borrow params output mismatch" >&2
    cat "$TMP_DIR/m9_borrow_params.out" >&2
    exit 1
fi

# M9.1: shared borrows may alias in one call; compound assignment through &mut.
printf 'addv a:&i64 b:&i64 -> a + b\nscale r:&mut i64 -> {\n    r *= 3\n}\nx <- 7\nprint(addv(&x, &x))\nscale(&mut x)\nprint(x)\n' > "$TMP_DIR/m9_borrow_shared.tiq"
./build/tiq build "$TMP_DIR/m9_borrow_shared.tiq" -o "$TMP_DIR/m9_borrow_shared" 2>"$TMP_DIR/m9_borrow_shared.err"
[ -x "$TMP_DIR/m9_borrow_shared" ]
"$TMP_DIR/m9_borrow_shared" > "$TMP_DIR/m9_borrow_shared.out"
printf '14\n21\n' > "$TMP_DIR/m9_borrow_shared.expected"
if ! cmp -s "$TMP_DIR/m9_borrow_shared.expected" "$TMP_DIR/m9_borrow_shared.out"; then
    echo "shared borrow output mismatch" >&2
    cat "$TMP_DIR/m9_borrow_shared.out" >&2
    exit 1
fi

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

# Named loop binders: [j <- 0..5] iterates j over the range; nested
# loops can pick distinct names and see enclosing binders.
printf 'total <- 0\n[j <- 0..5] { total += j }\nprint(total)\nnested <- 0\n[j <- 0..3] { [k <- 0..2] { nested += j * 10 + k } }\nprint(nested)\n' > "$TMP_DIR/loop_binder.tiq"
./build/tiq build "$TMP_DIR/loop_binder.tiq" -o "$TMP_DIR/loop_binder" 2>"$TMP_DIR/loop_binder.err"
[ -x "$TMP_DIR/loop_binder" ]
"$TMP_DIR/loop_binder" > "$TMP_DIR/loop_binder.out"
printf '10\n63\n' > "$TMP_DIR/loop_binder.expected"
if ! cmp -s "$TMP_DIR/loop_binder.expected" "$TMP_DIR/loop_binder.out"; then
  echo "loop binder output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/loop_binder.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/loop_binder.out" >&2
  exit 1
fi

# Multi-binder loops iterate the Cartesian product; later binders see
# earlier ones ([j <- 0..3, k <- 0..j] nests k inside j).
printf 'total <- 0\n[j <- 0..3, k <- 0..j] { total += j * 10 + k }\nprint(total)\n' > "$TMP_DIR/loop_multi_binder.tiq"
./build/tiq build "$TMP_DIR/loop_multi_binder.tiq" -o "$TMP_DIR/loop_multi_binder" 2>"$TMP_DIR/loop_multi_binder.err"
[ -x "$TMP_DIR/loop_multi_binder" ]
"$TMP_DIR/loop_multi_binder" > "$TMP_DIR/loop_multi_binder.out"
printf '51\n' > "$TMP_DIR/loop_multi_binder.expected"
if ! cmp -s "$TMP_DIR/loop_multi_binder.expected" "$TMP_DIR/loop_multi_binder.out"; then
  echo "multi-binder loop output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/loop_multi_binder.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/loop_multi_binder.out" >&2
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

# M12.3: explicit numeric type conversions.
# int -> f64: f64(42) prints as float
printf 'x = f64(42)\nprint(x)\n' > "$TMP_DIR/conversion_int_f64.tiq"
./build/tiq build "$TMP_DIR/conversion_int_f64.tiq" -o "$TMP_DIR/conversion_int_f64"
[ "$("$TMP_DIR/conversion_int_f64")" = "42" ]

# f64 -> i64: i64(3.9) truncates toward zero -> 3
printf 'x = i64(3.9)\nprint(x)\n' > "$TMP_DIR/conversion_f64_i64.tiq"
./build/tiq build "$TMP_DIR/conversion_f64_i64.tiq" -o "$TMP_DIR/conversion_f64_i64"
[ "$("$TMP_DIR/conversion_f64_i64")" = "3" ]

# Narrowing is explicit and allowed: i32(300) -> 300 (fits in i32)
printf 'x = i32(300)\nprint(x)\n' > "$TMP_DIR/conversion_narrowing.tiq"
./build/tiq build "$TMP_DIR/conversion_narrowing.tiq" -o "$TMP_DIR/conversion_narrowing"
[ "$("$TMP_DIR/conversion_narrowing")" = "300" ]

# Chain conversion: f64(i32(7)) -> 7
printf 'x = f64(i32(7))\nprint(x)\n' > "$TMP_DIR/conversion_chain.tiq"
./build/tiq build "$TMP_DIR/conversion_chain.tiq" -o "$TMP_DIR/conversion_chain"
[ "$("$TMP_DIR/conversion_chain")" = "7" ]

# f64 division: f64(count) / f64(total)
printf 'count <- 1\ntotal <- 4\nratio = f64(count) / f64(total)\nprint(ratio)\n' > "$TMP_DIR/conversion_ratio.tiq"
./build/tiq build "$TMP_DIR/conversion_ratio.tiq" -o "$TMP_DIR/conversion_ratio"
[ "$("$TMP_DIR/conversion_ratio")" = "0.25" ]

# Print sized types: i32, u8, f32
printf 'x = i32(42)\nprint(x)\n' > "$TMP_DIR/print_i32.tiq"
./build/tiq build "$TMP_DIR/print_i32.tiq" -o "$TMP_DIR/print_i32"
[ "$("$TMP_DIR/print_i32")" = "42" ]

printf 'x = u8(255)\nprint(x)\n' > "$TMP_DIR/print_u8.tiq"
./build/tiq build "$TMP_DIR/print_u8.tiq" -o "$TMP_DIR/print_u8"
[ "$("$TMP_DIR/print_u8")" = "255" ]

printf 'x = f32(1.5)\nprint(x)\n' > "$TMP_DIR/print_f32.tiq"
./build/tiq build "$TMP_DIR/print_f32.tiq" -o "$TMP_DIR/print_f32"
[ "$("$TMP_DIR/print_f32")" = "1.5" ]

# M12.6: Struct definitions, record literals, and field access.
printf 'struct Point {\n  x: i64,\n  y: i64\n}\np = Point { x: 1, y: 2 }\nprint(p.x)\nprint(p.y)\n' > "$TMP_DIR/struct_basic.tiq"
./build/tiq build "$TMP_DIR/struct_basic.tiq" -o "$TMP_DIR/struct_basic"
"$TMP_DIR/struct_basic" > "$TMP_DIR/struct_basic.out"
printf '1\n2\n' > "$TMP_DIR/struct_basic.expected"
if ! cmp -s "$TMP_DIR/struct_basic.expected" "$TMP_DIR/struct_basic.out"; then
  echo "struct basic output mismatch" >&2
  echo "expected:" >&2
  cat "$TMP_DIR/struct_basic.expected" >&2
  echo "actual:" >&2
  cat "$TMP_DIR/struct_basic.out" >&2
  exit 1
fi

# M8: Option types - some, none, and fallback operator.
printf 'x = some(42)\ny = x ?? 0\nprint(y)\n' > "$TMP_DIR/option_some.tiq"
./build/tiq build "$TMP_DIR/option_some.tiq" -o "$TMP_DIR/option_some"
[ "$("$TMP_DIR/option_some")" = "42" ]

printf 'x = none\ny = x ?? 99\nprint(y)\n' > "$TMP_DIR/option_none.tiq"
./build/tiq build "$TMP_DIR/option_none.tiq" -o "$TMP_DIR/option_none"
[ "$("$TMP_DIR/option_none")" = "99" ]

# M8: Result types - ok, err, and fallback operator.
printf 'x = ok(42)\ny = x ?? 0\nprint(y)\n' > "$TMP_DIR/result_ok.tiq"
./build/tiq build "$TMP_DIR/result_ok.tiq" -o "$TMP_DIR/result_ok"
[ "$("$TMP_DIR/result_ok")" = "42" ]

printf 'x = err(99)\ny = x ?? 0\nprint(y)\n' > "$TMP_DIR/result_err.tiq"
./build/tiq build "$TMP_DIR/result_err.tiq" -o "$TMP_DIR/result_err"
[ "$("$TMP_DIR/result_err")" = "0" ]

# M8/M25: Propagation operator (prefix `?expr`).
printf 'x = some(42)\ny = ?x\nprint(y)\n' > "$TMP_DIR/propagate_some.tiq"
./build/tiq build "$TMP_DIR/propagate_some.tiq" -o "$TMP_DIR/propagate_some"
[ "$("$TMP_DIR/propagate_some")" = "42" ]

# M10.1: CLI argument builtins read real argc/argv (LANGUAGE_SPEC §18.1);
# out-of-range indices yield the empty string.
printf 'print(cli_arg_count())\nprint(cli_arg(0))\nprint(cli_arg(1))\nprint(cli_arg(5))\n' > "$TMP_DIR/m10_cli_args.tiq"
./build/tiq build "$TMP_DIR/m10_cli_args.tiq" -o "$TMP_DIR/m10_cli_args"
"$TMP_DIR/m10_cli_args" alpha beta > "$TMP_DIR/m10_cli_args.out"
printf '2\nalpha\nbeta\n\n' > "$TMP_DIR/m10_cli_args.expected"
cmp "$TMP_DIR/m10_cli_args.out" "$TMP_DIR/m10_cli_args.expected"

printf 'print(cli_arg_count())\n' > "$TMP_DIR/m10_cli_none.tiq"
./build/tiq build "$TMP_DIR/m10_cli_none.tiq" -o "$TMP_DIR/m10_cli_none"
[ "$("$TMP_DIR/m10_cli_none")" = "0" ]

# LANGUAGE_SPEC §4: string escapes decode to their escaped characters.
cat > "$TMP_DIR/esc_decode.tiq" <<'EOF'
print("a\nb")
print("q\"q")
print("t\tt")
print("s\\s")
EOF
./build/tiq build "$TMP_DIR/esc_decode.tiq" -o "$TMP_DIR/esc_decode"
"$TMP_DIR/esc_decode" > "$TMP_DIR/esc_decode.out"
printf 'a\nb\nq"q\nt\tt\ns\\s\n' > "$TMP_DIR/esc_decode.expected"
cmp "$TMP_DIR/esc_decode.out" "$TMP_DIR/esc_decode.expected"

# M10.2: json_get decodes strings, returns raw scalars/sub-documents, and
# yields the empty string for missing keys or non-object input (LANGUAGE_SPEC §19).
cat > "$TMP_DIR/m10_json_get.tiq" <<'EOF'
import "std/json.tiq"
j = "{\"name\": \"tiq\", \"n\": 42, \"ok\": true, \"nested\": {\"x\": 7}, \"esc\": \"a\\nb\"}"
print(json_get(j, "name"))
print(json_get(j, "n"))
print(json_get(j, "ok"))
print(json_get(j, "nested"))
print(json_get(json_get(j, "nested"), "x"))
print(json_get(j, "esc"))
print(json_get(j, "missing"))
print(json_get("[1, 2]", "k"))
EOF
./build/tiq build "$TMP_DIR/m10_json_get.tiq" -o "$TMP_DIR/m10_json_get"
"$TMP_DIR/m10_json_get" > "$TMP_DIR/m10_json_get.out"
printf 'tiq\n42\ntrue\n{"x": 7}\n7\na\nb\n\n\n' > "$TMP_DIR/m10_json_get.expected"
cmp "$TMP_DIR/m10_json_get.out" "$TMP_DIR/m10_json_get.expected"

# M10.3: json_arr_len / json_arr_get read JSON arrays; out-of-range index,
# non-array input, and malformed input fail soft (LANGUAGE_SPEC §19).
cat > "$TMP_DIR/m10_json_arr.tiq" <<'EOF'
import "std/json.tiq"
a = "[10, \"two\", {\"x\": 5}, [1, 2], true]"
print(json_arr_len(a))
print(json_arr_get(a, 0))
print(json_arr_get(a, 1))
print(json_get(json_arr_get(a, 2), "x"))
print(json_arr_get(a, 3))
print(json_arr_len(json_arr_get(a, 3)))
print(json_arr_get(a, 4))
print(json_arr_get(a, 9))
print(json_arr_len("{}"))
print(json_arr_len("[]"))
print(json_arr_get("[]", 0))
EOF
./build/tiq build "$TMP_DIR/m10_json_arr.tiq" -o "$TMP_DIR/m10_json_arr"
"$TMP_DIR/m10_json_arr" > "$TMP_DIR/m10_json_arr.out"
printf '5\n10\ntwo\n5\n[1, 2]\n2\ntrue\n\n0\n0\n\n' > "$TMP_DIR/m10_json_arr.expected"
cmp "$TMP_DIR/m10_json_arr.out" "$TMP_DIR/m10_json_arr.expected"

# M10.4: net_fetch performs a real HTTP/1.0 GET (LANGUAGE_SPEC §19.2), tested
# against a local one-shot server on 127.0.0.1 (port 0, printed after bind).
# A non-http scheme and an empty host yield the empty string.
cat > "$TMP_DIR/http_server.c" <<'EOF'
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) return 1;
    if (listen(s, 1) != 0) return 1;
    socklen_t alen = sizeof addr;
    if (getsockname(s, (struct sockaddr *)&addr, &alen) != 0) return 1;
    printf("%d\n", (int)ntohs(addr.sin_port));
    fflush(stdout);
    int c = accept(s, 0, 0);
    if (c < 0) return 1;
    char buf[1024];
    (void)read(c, buf, sizeof buf);
    const char *resp = "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/json\r\n\r\n"
        "{\"greet\": \"hello tiq\"}";
    (void)write(c, resp, strlen(resp));
    close(c);
    close(s);
    return 0;
}
EOF
cc -std=c11 -o "$TMP_DIR/http_server" "$TMP_DIR/http_server.c"

# 10s alarm converts a hung accept/read into a test failure.
perl -e 'alarm 10; exec @ARGV' "$TMP_DIR/http_server" > "$TMP_DIR/http_port" &
HTTP_SERVER_PID=$!
PORT=""
i=0
while [ "$i" -lt 100 ]; do
  PORT="$(cat "$TMP_DIR/http_port" 2>/dev/null || true)"
  [ -n "$PORT" ] && break
  sleep 0.1
  i=$((i + 1))
done
[ -n "$PORT" ]

cat > "$TMP_DIR/m10_net_fetch.tiq" <<'EOF'
import "std/net.tiq"
import "std/json.tiq"
body = net_fetch(cli_arg(0))
print(json_get(body, "greet"))
print(net_fetch("ftp://example.invalid/"))
print(net_fetch("http://"))
EOF
./build/tiq build "$TMP_DIR/m10_net_fetch.tiq" -o "$TMP_DIR/m10_net_fetch"
"$TMP_DIR/m10_net_fetch" "http://127.0.0.1:$PORT/" > "$TMP_DIR/m10_net_fetch.out"
wait "$HTTP_SERVER_PID"
printf 'hello tiq\n\n\n' > "$TMP_DIR/m10_net_fetch.expected"
cmp "$TMP_DIR/m10_net_fetch.out" "$TMP_DIR/m10_net_fetch.expected"

# M10.5: net_fetch speaks HTTP/1.1 and decodes chunked responses
# (LANGUAGE_SPEC §19.2). The one-shot server replies chunked (lowercase
# header, a chunk-size extension, a trailer) only when the request line
# says HTTP/1.1; otherwise it sends a "wrong version" body.
cat > "$TMP_DIR/m105_server.c" <<'EOF'
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) return 1;
    if (listen(s, 1) != 0) return 1;
    socklen_t alen = sizeof addr;
    if (getsockname(s, (struct sockaddr *)&addr, &alen) != 0) return 1;
    printf("%d\n", (int)ntohs(addr.sin_port));
    fflush(stdout);
    int c = accept(s, 0, 0);
    if (c < 0) return 1;
    char buf[1024];
    ssize_t n = read(c, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    const char *ok = "HTTP/1.1 200 OK\r\n"
        "transfer-encoding: chunked\r\n\r\n"
        "d;ext=1\r\n{\"greet\": \"he\r\n"
        "d\r\nllo chunked\"}\r\n"
        "0\r\nX-Trail: 1\r\n\r\n";
    const char *bad = "HTTP/1.1 200 OK\r\n\r\n{\"greet\": \"wrong version\"}";
    const char *resp = strncmp(buf, "GET / HTTP/1.1\r\n", 16) == 0 ? ok : bad;
    (void)write(c, resp, strlen(resp));
    close(c);
    close(s);
    return 0;
}
EOF
cc -std=c11 -o "$TMP_DIR/m105_server" "$TMP_DIR/m105_server.c"

perl -e 'alarm 10; exec @ARGV' "$TMP_DIR/m105_server" > "$TMP_DIR/m105_port" &
M105_SERVER_PID=$!
PORT=""
i=0
while [ "$i" -lt 100 ]; do
  PORT="$(cat "$TMP_DIR/m105_port" 2>/dev/null || true)"
  [ -n "$PORT" ] && break
  sleep 0.1
  i=$((i + 1))
done
[ -n "$PORT" ]

cat > "$TMP_DIR/m105_chunked.tiq" <<'EOF'
import "std/net.tiq"
import "std/json.tiq"
body = net_fetch(cli_arg(0))
print(json_get(body, "greet"))
EOF
./build/tiq build "$TMP_DIR/m105_chunked.tiq" -o "$TMP_DIR/m105_chunked"
"$TMP_DIR/m105_chunked" "http://127.0.0.1:$PORT/" > "$TMP_DIR/m105_chunked.out"
wait "$M105_SERVER_PID"
printf 'hello chunked\n' > "$TMP_DIR/m105_chunked.expected"
cmp "$TMP_DIR/m105_chunked.out" "$TMP_DIR/m105_chunked.expected"

# M9.2: owned heap strings are freed at scope end, reverse declaration order,
# after defers (LANGUAGE_SPEC §16.4); aliases and cli_arg results are not
# freed. ASan build of the emitted C catches double-free / use-after-free.
cat > "$TMP_DIR/m92_scope.tiq" <<'EOF'
import "std/json.tiq"
a = json_get("{\"x\": \"one\"}", "x")
b = json_encode_str("two")
c = a
d = cli_arg(0)
print(a)
print(b)
print(c)
print(d)
{
    e = json_get("{\"k\": \"v\"}", "k")
    defer print(e)
    print(1)
}
[i <- 0..2] {
    f = json_get("{\"n\": 7}", "n")
    print(f)
}
EOF
./build/tiq emit-c "$TMP_DIR/m92_scope.tiq" > "$TMP_DIR/m92_scope.c"
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/m92_scope.c" > "$TMP_DIR/m92_scope.frees" || true
printf 'free((void *)e);\nfree((void *)f);\nfree((void *)b);\nfree((void *)a);\n' > "$TMP_DIR/m92_scope.frees.expected"
cmp "$TMP_DIR/m92_scope.frees" "$TMP_DIR/m92_scope.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92_scope_asan" "$TMP_DIR/m92_scope.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92_scope_asan" > "$TMP_DIR/m92_scope.out"
printf 'one\n"two"\none\n\n1\nv\n7\n7\n' > "$TMP_DIR/m92_scope.expected"
cmp "$TMP_DIR/m92_scope.out" "$TMP_DIR/m92_scope.expected"

# M9.2-B: break/skip destroy the owned strings of every scope they exit
# (innermost first, through the enclosing loop body) before jumping; only
# owners already bound at the jump point are freed (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92b_early.tiq" <<'EOF'
import "std/json.tiq"
[i <- 0..3] {
    a = json_get("{\"n\": 7}", "n")
    [(i == 0)] {
        b = json_get("{\"m\": 5}", "m")
        print(b)
        break
    }
    print(a)
    break
    d = json_get("{\"q\": 0}", "q")
}
[i <- 0..2] {
    f = json_get("{\"k\": 3}", "k")
    print(f)
    skip
    print(999)
}
[i <- 0..2] {
    g = json_get("{\"g\": 1}", "g")
    {
        h = json_get("{\"h\": 2}", "h")
        print(h)
        break
    }
    print(g)
}
EOF
./build/tiq emit-c "$TMP_DIR/m92b_early.tiq" > "$TMP_DIR/m92b_early.c"
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/m92b_early.c" > "$TMP_DIR/m92b_early.frees" || true
printf 'free((void *)b);\nfree((void *)b);\nfree((void *)a);\nfree((void *)d);\nfree((void *)a);\nfree((void *)f);\nfree((void *)f);\nfree((void *)h);\nfree((void *)g);\nfree((void *)h);\nfree((void *)g);\n' > "$TMP_DIR/m92b_early.frees.expected"
cmp "$TMP_DIR/m92b_early.frees" "$TMP_DIR/m92b_early.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92b_early_asan" "$TMP_DIR/m92b_early.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92b_early_asan" > "$TMP_DIR/m92b_early.out"
printf '5\n7\n3\n3\n2\n' > "$TMP_DIR/m92b_early.expected"
cmp "$TMP_DIR/m92b_early.out" "$TMP_DIR/m92b_early.expected"

# M9.2-C: a scalar-result function frees its body's owned strings after the
# result is computed; a str-result function must not free (the result may
# alias an owner) (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92c_fn.tiq" <<'EOF'
import "std/json.tiq"
get_n src:str -> {
    v = json_get(src, "n")
    json_parse_int(v)
}
get_s src:str -> str -> {
    w = json_get(src, "s")
    w
}
print(get_n("{\"n\": 42}"))
print(get_s("{\"s\": \"ok\"}"))
EOF
./build/tiq emit-c "$TMP_DIR/m92c_fn.tiq" > "$TMP_DIR/m92c_fn.c"
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/m92c_fn.c" > "$TMP_DIR/m92c_fn.frees" || true
printf 'free((void *)v);\n' > "$TMP_DIR/m92c_fn.frees.expected"
cmp "$TMP_DIR/m92c_fn.frees" "$TMP_DIR/m92c_fn.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92c_fn_asan" "$TMP_DIR/m92c_fn.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92c_fn_asan" > "$TMP_DIR/m92c_fn.out"
printf '42\nok\n' > "$TMP_DIR/m92c_fn.expected"
cmp "$TMP_DIR/m92c_fn.out" "$TMP_DIR/m92c_fn.expected"

# M9.2-D: a mutable owner (all assignments are owned-builtin calls, name only
# used as builtin argument) frees its previous string on reassignment and is
# freed at scope end; an aliased mutable is disqualified and never freed
# (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92d_mut.tiq" <<'EOF'
import "std/json.tiq"
s <- json_get("{\"a\": \"one\"}", "a")
print(s)
s <- json_get("{\"b\": \"two\"}", "b")
print(s)
[i <- 0..2] {
    s <- json_get("{\"c\": \"three\"}", "c")
    print(s)
}
t <- json_get("{\"d\": \"four\"}", "d")
u = t
print(u)
EOF
./build/tiq emit-c "$TMP_DIR/m92d_mut.tiq" > "$TMP_DIR/m92d_mut.c"
grep -o 'free((void \*)[a-z_]*);' "$TMP_DIR/m92d_mut.c" > "$TMP_DIR/m92d_mut.frees" || true
printf 'free((void *)tiq_old);\nfree((void *)tiq_old);\nfree((void *)s);\n' > "$TMP_DIR/m92d_mut.frees.expected"
cmp "$TMP_DIR/m92d_mut.frees" "$TMP_DIR/m92d_mut.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92d_mut_asan" "$TMP_DIR/m92d_mut.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92d_mut_asan" > "$TMP_DIR/m92d_mut.out"
printf 'one\ntwo\nthree\nthree\nfour\n' > "$TMP_DIR/m92d_mut.expected"
cmp "$TMP_DIR/m92d_mut.out" "$TMP_DIR/m92d_mut.expected"

# M9.2-E: a statement-level proc_exit destroys the owned strings of every
# enclosing scope (innermost first) before terminating; the exit code is
# computed before destruction (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92e_exit.tiq" <<'EOF'
import "std/json.tiq"
a = json_get("{\"a\": \"one\"}", "a")
print(a)
[i <- 0..3] {
    b = json_get("{\"b\": \"two\"}", "b")
    print(b)
    [(i == 2)] {
        proc_exit(7)
    }
}
EOF
./build/tiq emit-c "$TMP_DIR/m92e_exit.tiq" > "$TMP_DIR/m92e_exit.c"
grep -o 'free((void \*)[a-z_]*);' "$TMP_DIR/m92e_exit.c" > "$TMP_DIR/m92e_exit.frees" || true
printf 'free((void *)b);\nfree((void *)a);\nfree((void *)b);\nfree((void *)a);\n' > "$TMP_DIR/m92e_exit.frees.expected"
cmp "$TMP_DIR/m92e_exit.frees" "$TMP_DIR/m92e_exit.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92e_exit_asan" "$TMP_DIR/m92e_exit.c"
exit_status=0
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92e_exit_asan" > "$TMP_DIR/m92e_exit.out" || exit_status=$?
test "$exit_status" -eq 7
printf 'one\ntwo\ntwo\ntwo\n' > "$TMP_DIR/m92e_exit.expected"
cmp "$TMP_DIR/m92e_exit.out" "$TMP_DIR/m92e_exit.expected"

# M9.2-F: an unbound owned-builtin temporary in an unconditionally evaluated
# position of a simple statement (bare statement expression, or argument to a
# standard-library builtin) is hoisted into a hidden binding and freed at the
# end of the statement; a user-function argument is not (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92f_tmp.tiq" <<'EOF'
import "std/json.tiq"
id x:str -> str -> {
    x
}
print(json_get("{\"k\": \"one\"}", "k"))
n = json_parse_int(json_get("{\"n\": 7}", "n"))
print(n)
json_encode_str("two")
y = id(json_get("{\"k\": \"three\"}", "k"))
print(y)
EOF
./build/tiq emit-c "$TMP_DIR/m92f_tmp.tiq" > "$TMP_DIR/m92f_tmp.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92f_tmp.c" > "$TMP_DIR/m92f_tmp.frees" || true
printf 'free((void *)tiq_tmp0);\nfree((void *)tiq_tmp1);\nfree((void *)tiq_tmp2);\n' > "$TMP_DIR/m92f_tmp.frees.expected"
cmp "$TMP_DIR/m92f_tmp.frees" "$TMP_DIR/m92f_tmp.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92f_tmp_asan" "$TMP_DIR/m92f_tmp.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92f_tmp_asan" > "$TMP_DIR/m92f_tmp.out"
printf 'one\n7\nthree\n' > "$TMP_DIR/m92f_tmp.expected"
cmp "$TMP_DIR/m92f_tmp.out" "$TMP_DIR/m92f_tmp.expected"

# M9.2-G: a str-result function whose final expression is a string literal or
# a direct owned-builtin call cannot alias a body owner, so it frees its
# owners after computing the result; an identifier result still must not free
# (it may alias an owner) (LANGUAGE_SPEC §16.4). Since M9.2-J the two
# fresh-result calls in argument position are hoisted and freed by the caller.
cat > "$TMP_DIR/m92g_strfn.tiq" <<'EOF'
import "std/json.tiq"
make_s src:str -> str -> {
    v = json_get(src, "s")
    json_encode_str(v)
}
lit_s src:str -> str -> {
    w = json_get(src, "s")
    "lit"
}
alias_s src:str -> str -> {
    u = json_get(src, "s")
    u
}
print(make_s("{\"s\": \"hi\"}"))
print(lit_s("{\"s\": \"no\"}"))
print(alias_s("{\"s\": \"ok\"}"))
EOF
./build/tiq emit-c "$TMP_DIR/m92g_strfn.tiq" > "$TMP_DIR/m92g_strfn.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92g_strfn.c" > "$TMP_DIR/m92g_strfn.frees" || true
printf 'free((void *)tiq_tmp0);\nfree((void *)tiq_tmp1);\nfree((void *)v);\nfree((void *)w);\n' > "$TMP_DIR/m92g_strfn.frees.expected"
cmp "$TMP_DIR/m92g_strfn.frees" "$TMP_DIR/m92g_strfn.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92g_strfn_asan" "$TMP_DIR/m92g_strfn.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92g_strfn_asan" > "$TMP_DIR/m92g_strfn.out"
printf '"hi"\nlit\nok\n' > "$TMP_DIR/m92g_strfn.expected"
cmp "$TMP_DIR/m92g_strfn.out" "$TMP_DIR/m92g_strfn.expected"

# M9.2-H: a str-result function whose final expression is a bare identifier
# naming a body owner transfers that owner to the caller: every *other* owner
# is freed after the result is computed; the named owner is returned
# undestroyed. An alias-identifier result still must not free anything
# (LANGUAGE_SPEC §16.4). Since M9.2-J the transferring call in argument
# position is hoisted and freed by the caller; the alias-returning call is not.
cat > "$TMP_DIR/m92h_xfer.tiq" <<'EOF'
import "std/json.tiq"
pick src:str -> str -> {
    a = json_get(src, "a")
    b = json_get(src, "b")
    b
}
alias_ret src:str -> str -> {
    c = json_get(src, "c")
    d = c
    d
}
print(pick("{\"a\": \"xx\", \"b\": \"yy\"}"))
print(alias_ret("{\"c\": \"zz\"}"))
EOF
./build/tiq emit-c "$TMP_DIR/m92h_xfer.tiq" > "$TMP_DIR/m92h_xfer.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92h_xfer.c" > "$TMP_DIR/m92h_xfer.frees" || true
printf 'free((void *)tiq_tmp0);\nfree((void *)a);\n' > "$TMP_DIR/m92h_xfer.frees.expected"
cmp "$TMP_DIR/m92h_xfer.frees" "$TMP_DIR/m92h_xfer.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92h_xfer_asan" "$TMP_DIR/m92h_xfer.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92h_xfer_asan" > "$TMP_DIR/m92h_xfer.out"
printf 'yy\nzz\n' > "$TMP_DIR/m92h_xfer.expected"
cmp "$TMP_DIR/m92h_xfer.out" "$TMP_DIR/m92h_xfer.expected"

# M9.2-I: a binding initialized from a direct call to a fresh-result function
# (result expression is a heap-builtin call or a bare identifier naming a body
# owner) owns the returned string and is freed at scope end; a function
# returning a string literal is not fresh-result, so its result is never freed
# (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92i_call.tiq" <<'EOF'
import "std/json.tiq"
mk src:str -> str -> {
    a = json_get(src, "a")
    json_encode_str(a)
}
pick src:str -> str -> {
    b = json_get(src, "b")
    c = json_get(src, "c")
    c
}
lit src:str -> str -> {
    "static"
}
v = mk("{\"a\": \"hi\"}")
u = pick("{\"b\": \"1\", \"c\": \"yy\"}")
w = lit("z")
print(v)
print(u)
print(w)
EOF
./build/tiq emit-c "$TMP_DIR/m92i_call.tiq" > "$TMP_DIR/m92i_call.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92i_call.c" > "$TMP_DIR/m92i_call.frees" || true
printf 'free((void *)u);\nfree((void *)v);\nfree((void *)a);\nfree((void *)b);\n' > "$TMP_DIR/m92i_call.frees.expected"
cmp "$TMP_DIR/m92i_call.frees" "$TMP_DIR/m92i_call.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92i_call_asan" "$TMP_DIR/m92i_call.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92i_call_asan" > "$TMP_DIR/m92i_call.out"
printf '"hi"\nyy\nstatic\n' > "$TMP_DIR/m92i_call.expected"
cmp "$TMP_DIR/m92i_call.out" "$TMP_DIR/m92i_call.expected"

# M9.2-J: an unbound fresh-result function call in an unconditionally evaluated
# position is hoisted into a hidden binding and freed at the end of its
# statement, and arguments of a fresh-result call are such positions too (its
# result cannot alias them); a call to a function that is not fresh-result is
# still not a hoist position (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92j_tmp.tiq" <<'EOF'
import "std/json.tiq"
mk src:str -> str -> {
    a = json_get(src, "a")
    json_encode_str(a)
}
wrap s:str -> str -> {
    json_encode_str(s)
}
pick src:str -> str -> {
    b = json_get(src, "b")
    b
}
keep x:str -> str -> {
    x
}
mk("{\"a\": \"one\"}")
print(pick("{\"b\": \"two\"}"))
print(wrap(pick("{\"b\": \"three\"}")))
z = keep(json_get("{\"k\": \"four\"}", "k"))
print(z)
EOF
./build/tiq emit-c "$TMP_DIR/m92j_tmp.tiq" > "$TMP_DIR/m92j_tmp.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92j_tmp.c" > "$TMP_DIR/m92j_tmp.frees" || true
printf 'free((void *)tiq_tmp0);\nfree((void *)tiq_tmp1);\nfree((void *)tiq_tmp3);\nfree((void *)tiq_tmp2);\nfree((void *)a);\n' > "$TMP_DIR/m92j_tmp.frees.expected"
cmp "$TMP_DIR/m92j_tmp.frees" "$TMP_DIR/m92j_tmp.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92j_tmp_asan" "$TMP_DIR/m92j_tmp.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92j_tmp_asan" > "$TMP_DIR/m92j_tmp.out"
printf 'two\n"three"\nfour\n' > "$TMP_DIR/m92j_tmp.expected"
cmp "$TMP_DIR/m92j_tmp.out" "$TMP_DIR/m92j_tmp.expected"

# M10.6: json_view(json, key) returns a zero-copy str view (TiqSlice) into the
# source buffer: raw string bytes without quotes/escape decoding, verbatim
# scalar tokens, raw sub-documents; missing key yields an empty view.
cat > "$TMP_DIR/m106_jview.tiq" <<'EOF'
import "std/json.tiq"
j = "{\"name\": \"hello\", \"age\": 42, \"nested\": {\"a\": 1}}"
v = json_view(j, "name")
print(v)
print(len(v))
w = json_view(j, "age")
print(w)
x = json_view(j, "missing")
print(len(x))
y = json_view(j, "nested")
print(y)
EOF
./build/tiq emit-c "$TMP_DIR/m106_jview.tiq" > "$TMP_DIR/m106_jview.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m106_jview.c" > "$TMP_DIR/m106_jview.frees" || true
test ! -s "$TMP_DIR/m106_jview.frees"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m106_jview_asan" "$TMP_DIR/m106_jview.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m106_jview_asan" > "$TMP_DIR/m106_jview.out"
printf 'hello\n5\n42\n0\n{"a": 1}\n' > "$TMP_DIR/m106_jview.expected"
cmp "$TMP_DIR/m106_jview.out" "$TMP_DIR/m106_jview.expected"

# M9.2-K: a binding whose initializer is a conditional expression owns the
# result when both branches are owning expressions (heap-builtin calls,
# fresh-result calls, or nested conditionals thereof); a conditional with a
# non-owning branch (e.g. a literal) does not create an owner.
cat > "$TMP_DIR/m92k_cond.tiq" <<'EOF'
import "std/json.tiq"
mk src:str -> str -> {
    a = json_get(src, "a")
    json_encode_str(a)
}
b = 1 > 0 ? json_get("{\"k\": \"yes\"}", "k") : json_get("{\"k\": \"no\"}", "k")
print(b)
c = 1 > 0 ? mk("{\"a\": \"one\"}") : json_get("{\"k\": \"two\"}", "k")
print(c)
d = 1 > 0 ? "lit" : json_get("{\"k\": \"x\"}", "k")
print(d)
EOF
./build/tiq emit-c "$TMP_DIR/m92k_cond.tiq" > "$TMP_DIR/m92k_cond.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92k_cond.c" > "$TMP_DIR/m92k_cond.frees" || true
printf 'free((void *)c);\nfree((void *)b);\nfree((void *)a);\n' > "$TMP_DIR/m92k_cond.frees.expected"
cmp "$TMP_DIR/m92k_cond.frees" "$TMP_DIR/m92k_cond.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92k_cond_asan" "$TMP_DIR/m92k_cond.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92k_cond_asan" > "$TMP_DIR/m92k_cond.out"
printf 'yes\n"one"\nlit\n' > "$TMP_DIR/m92k_cond.expected"
cmp "$TMP_DIR/m92k_cond.out" "$TMP_DIR/m92k_cond.expected"

# M10.7: json_has(json, key) returns true when the key exists (even if the
# value is the empty string) and false for missing keys / non-objects.
cat > "$TMP_DIR/m107_jhas.tiq" <<'EOF'
import "std/json.tiq"
j = "{\"a\": \"\", \"b\": 42}"
print(json_has(j, "a"))
print(json_has(j, "b"))
print(json_has(j, "c"))
print(json_has("[1, 2]", "a"))
EOF
./build/tiq emit-c "$TMP_DIR/m107_jhas.tiq" > "$TMP_DIR/m107_jhas.c"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m107_jhas_asan" "$TMP_DIR/m107_jhas.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m107_jhas_asan" > "$TMP_DIR/m107_jhas.out"
printf 'true\ntrue\nfalse\nfalse\n' > "$TMP_DIR/m107_jhas.expected"
cmp "$TMP_DIR/m107_jhas.out" "$TMP_DIR/m107_jhas.expected"

# M9.2-L: conditional-position temporaries — a conditional expression whose
# branches are both owning expressions, appearing as a direct argument to a
# builtin or as the bare statement expression, is hoisted into a hidden
# binding and freed at statement end; a conditional with a non-owning branch
# is not hoisted (leaks, never dangles).
cat > "$TMP_DIR/m92l_cond_tmp.tiq" <<'EOF'
import "std/json.tiq"
mk src:str -> str -> {
    json_get(src, "v")
}
a = json_get("{\"k\": \"hello\"}", "k")
print(len(1 > 0 ? json_get("{\"a\": \"xy\"}", "a") : json_get("{\"b\": \"z\"}", "b")))
print(len(1 > 0 ? mk("{\"v\": \"abc\"}") : json_get("{\"b\": \"w\"}", "b")))
1 > 0 ? json_get("{\"c\": \"bare\"}", "c") : json_get("{\"d\": \"q\"}", "d")
print(len(1 > 0 ? "lit" : json_get("{\"e\": \"r\"}", "e")))
print(a)
EOF
./build/tiq emit-c "$TMP_DIR/m92l_cond_tmp.tiq" > "$TMP_DIR/m92l_cond_tmp.c"
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/m92l_cond_tmp.c" > "$TMP_DIR/m92l_cond_tmp.frees" || true
printf 'free((void *)tiq_tmp0);\nfree((void *)tiq_tmp1);\nfree((void *)tiq_tmp2);\nfree((void *)a);\n' > "$TMP_DIR/m92l_cond_tmp.frees.expected"
cmp "$TMP_DIR/m92l_cond_tmp.frees" "$TMP_DIR/m92l_cond_tmp.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92l_cond_tmp_asan" "$TMP_DIR/m92l_cond_tmp.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/m92l_cond_tmp_asan" > "$TMP_DIR/m92l_cond_tmp.out"
printf '2\n3\n3\nhello\n' > "$TMP_DIR/m92l_cond_tmp.expected"
cmp "$TMP_DIR/m92l_cond_tmp.out" "$TMP_DIR/m92l_cond_tmp.expected"

# M10.8: TCP socket primitives — a Tiq server (net_listen/net_accept/net_recv/
# net_send/net_close) serves one HTTP response to a net_fetch client.
cat > "$TMP_DIR/m108_srv.tiq" <<'EOF'
import "std/net.tiq"
fd = net_listen(18923)
c = net_accept(fd)
req = net_recv(c)
net_send(c, "HTTP/1.0 200 OK\r\nContent-Length: 13\r\n\r\n{\"ok\": true}")
net_close(c)
net_close(fd)
EOF
cat > "$TMP_DIR/m108_cli.tiq" <<'EOF'
import "std/net.tiq"
import "std/json.tiq"
r = net_fetch("http://127.0.0.1:18923/")
print(json_get(r, "ok"))
EOF
./build/tiq build "$TMP_DIR/m108_srv.tiq" -o "$TMP_DIR/m108_srv"
./build/tiq build "$TMP_DIR/m108_cli.tiq" -o "$TMP_DIR/m108_cli"
"$TMP_DIR/m108_srv" &
M108_PID=$!
sleep 0.5
"$TMP_DIR/m108_cli" > "$TMP_DIR/m108_cli.out" 2>/dev/null || true
kill "$M108_PID" 2>/dev/null || true
wait "$M108_PID" 2>/dev/null || true
printf 'true\n' > "$TMP_DIR/m108_cli.expected"
cmp "$TMP_DIR/m108_cli.out" "$TMP_DIR/m108_cli.expected"

# M10.9: HTTP request-line parsing — http_method and http_path extract the
# method and path tokens from an HTTP request line; owned heap strings.
cat > "$TMP_DIR/m109_http.tiq" <<'EOF'
import "std/net.tiq"
req = "GET /index.html HTTP/1.1"
print(http_method(req))
print(http_path(req))
print(http_method("POST"))
print(http_path("POST"))
print(http_method(""))
print(http_path(""))
EOF
./build/tiq build "$TMP_DIR/m109_http.tiq" -o "$TMP_DIR/m109_http"
"$TMP_DIR/m109_http" > "$TMP_DIR/m109_http.out"
printf 'GET\n/index.html\nPOST\n\n\n\n' > "$TMP_DIR/m109_http.expected"
cmp "$TMP_DIR/m109_http.out" "$TMP_DIR/m109_http.expected"

# M10.10: Event loop (kqueue) — a Tiq server uses ev_loop/ev_add/ev_wait/
# ev_ready to detect and serve one client connection without blocking on
# accept; a net_fetch client verifies the response.
cat > "$TMP_DIR/m1010_srv.tiq" <<'EOF'
import "std/ev.tiq"
import "std/net.tiq"
fd = net_listen(18924)
lp = ev_loop()
ev_add(lp, fd)
ev_wait(lp, 5000)
c = net_accept(fd)
ev_add(lp, c)
ev_wait(lp, 5000)
req = net_recv(c)
net_send(c, "HTTP/1.0 200 OK\r\nContent-Length: 14\r\n\r\n{\"ev\": true}")
net_close(c)
net_close(fd)
EOF
cat > "$TMP_DIR/m1010_cli.tiq" <<'EOF'
import "std/net.tiq"
import "std/json.tiq"
r = net_fetch("http://127.0.0.1:18924/")
print(json_get(r, "ev"))
EOF
./build/tiq build "$TMP_DIR/m1010_srv.tiq" -o "$TMP_DIR/m1010_srv"
./build/tiq build "$TMP_DIR/m1010_cli.tiq" -o "$TMP_DIR/m1010_cli"
"$TMP_DIR/m1010_srv" &
M1010_PID=$!
sleep 0.5
"$TMP_DIR/m1010_cli" > "$TMP_DIR/m1010_cli.out" 2>/dev/null || true
kill "$M1010_PID" 2>/dev/null || true
wait "$M1010_PID" 2>/dev/null || true
printf 'true\n' > "$TMP_DIR/m1010_cli.expected"
cmp "$TMP_DIR/m1010_cli.out" "$TMP_DIR/m1010_cli.expected"

# M10.11: json_set — build and modify JSON objects; owned heap result.
cat > "$TMP_DIR/m1011_jset.tiq" <<'EOF'
import "std/json.tiq"
a = json_set("{}", "name", json_encode_str("tiq"))
print(json_get(a, "name"))
b = json_set(a, "ver", "1")
print(json_get(b, "ver"))
c = json_set(b, "name", json_encode_str("lang"))
print(json_get(c, "name"))
d = json_set("bad", "k", "42")
print(json_get(d, "k"))
EOF
./build/tiq build "$TMP_DIR/m1011_jset.tiq" -o "$TMP_DIR/m1011_jset"
"$TMP_DIR/m1011_jset" > "$TMP_DIR/m1011_jset.out"
printf 'tiq\n1\nlang\n42\n' > "$TMP_DIR/m1011_jset.expected"
cmp "$TMP_DIR/m1011_jset.out" "$TMP_DIR/m1011_jset.expected"

# M10.12: json_del — remove members from JSON objects; owned heap result.
cat > "$TMP_DIR/m1012_jdel.tiq" <<'EOF'
import "std/json.tiq"
a = json_set("{}", "x", "1")
b = json_set(a, "y", "2")
c = json_del(b, "x")
print(json_get(c, "x"))
print(json_get(c, "y"))
d = json_del(c, "y")
print(json_get(d, "y"))
e = json_del("bad", "k")
print(len(e))
EOF
./build/tiq build "$TMP_DIR/m1012_jdel.tiq" -o "$TMP_DIR/m1012_jdel"
"$TMP_DIR/m1012_jdel" > "$TMP_DIR/m1012_jdel.out"
printf '\n2\n\n3\n' > "$TMP_DIR/m1012_jdel.expected"
cmp "$TMP_DIR/m1012_jdel.out" "$TMP_DIR/m1012_jdel.expected"

# M10.13: str_cat — string concatenation; owned heap result.
cat > "$TMP_DIR/m1013_scat.tiq" <<'EOF'
a = str_cat("hello", " world")
print(a)
print(str_cat("", "x"))
print(str_cat("y", ""))
print(len(str_cat("ab", "cd")))
EOF
./build/tiq build "$TMP_DIR/m1013_scat.tiq" -o "$TMP_DIR/m1013_scat"
"$TMP_DIR/m1013_scat" > "$TMP_DIR/m1013_scat.out"
printf 'hello world\nx\ny\n4\n' > "$TMP_DIR/m1013_scat.expected"
cmp "$TMP_DIR/m1013_scat.out" "$TMP_DIR/m1013_scat.expected"

# M10.14: int_str — integer to decimal string; owned heap result.
cat > "$TMP_DIR/m1014_istr.tiq" <<'EOF'
print(int_str(42))
print(int_str(-7))
print(int_str(0))
print(len(int_str(12345)))
EOF
./build/tiq build "$TMP_DIR/m1014_istr.tiq" -o "$TMP_DIR/m1014_istr"
"$TMP_DIR/m1014_istr" > "$TMP_DIR/m1014_istr.out"
printf '42\n-7\n0\n5\n' > "$TMP_DIR/m1014_istr.expected"
cmp "$TMP_DIR/m1014_istr.out" "$TMP_DIR/m1014_istr.expected"

# M10.15: http_header — extract header values from HTTP requests.
cat > "$TMP_DIR/m1015_hdr.tiq" <<'EOF'
import "std/net.tiq"
req = "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/html\r\n\r\n"
print(http_header(req, "Host"))
print(http_header(req, "Content-Type"))
print(http_header(req, "Missing"))
EOF
./build/tiq build "$TMP_DIR/m1015_hdr.tiq" -o "$TMP_DIR/m1015_hdr"
"$TMP_DIR/m1015_hdr" > "$TMP_DIR/m1015_hdr.out"
printf 'localhost\ntext/html\n\n' > "$TMP_DIR/m1015_hdr.expected"
cmp "$TMP_DIR/m1015_hdr.out" "$TMP_DIR/m1015_hdr.expected"

# M13.1-P1: str_sub — byte substring [start, end); any invalid range
# (start < 0, end < start, end > len) yields the empty string, never a
# runtime error (LANGUAGE_SPEC §19.5).
cat > "$TMP_DIR/p1_str_sub.tiq" <<'EOF'
s = "Hello World"
print(str_sub(s, 0, 5))
print(str_sub(s, 6, 11))
print(str_sub(s, 0, 11))
print(len(str_sub(s, 3, 3)))
print(len(str_sub(s, -1, 4)))
print(len(str_sub(s, 4, 2)))
print(len(str_sub(s, 0, 12)))
print(len(str_sub(s, 12, 15)))
print(len(str_sub("", 0, 0)))
EOF
./build/tiq build "$TMP_DIR/p1_str_sub.tiq" -o "$TMP_DIR/p1_str_sub"
"$TMP_DIR/p1_str_sub" > "$TMP_DIR/p1_str_sub.out"
printf 'Hello\nWorld\nHello World\n0\n0\n0\n0\n0\n0\n' > "$TMP_DIR/p1_str_sub.expected"
cmp "$TMP_DIR/p1_str_sub.out" "$TMP_DIR/p1_str_sub.expected"

# M13.1-P1: str_eq — byte equality, no allocation (LANGUAGE_SPEC §19.5).
cat > "$TMP_DIR/p1_str_eq.tiq" <<'EOF'
print(str_eq("abc", "abc"))
print(str_eq("abc", "abd"))
print(str_eq("", ""))
print(str_eq("a", ""))
print(str_eq("a", "ab"))
EOF
./build/tiq build "$TMP_DIR/p1_str_eq.tiq" -o "$TMP_DIR/p1_str_eq"
"$TMP_DIR/p1_str_eq" > "$TMP_DIR/p1_str_eq.out"
printf 'true\nfalse\ntrue\nfalse\nfalse\n' > "$TMP_DIR/p1_str_eq.expected"
cmp "$TMP_DIR/p1_str_eq.out" "$TMP_DIR/p1_str_eq.expected"

# M13.1-P1: eprint — identical formatting to print's str case (value plus
# trailing newline) but on stderr; returns bytes written (LANGUAGE_SPEC §12).
cat > "$TMP_DIR/p1_eprint.tiq" <<'EOF'
n = eprint("to-stderr")
print(n)
EOF
./build/tiq build "$TMP_DIR/p1_eprint.tiq" -o "$TMP_DIR/p1_eprint"
"$TMP_DIR/p1_eprint" > "$TMP_DIR/p1_eprint.out" 2> "$TMP_DIR/p1_eprint.err"
printf 'to-stderr\n' > "$TMP_DIR/p1_eprint.err.expected"
cmp "$TMP_DIR/p1_eprint.err" "$TMP_DIR/p1_eprint.err.expected"
printf '10\n' > "$TMP_DIR/p1_eprint.out.expected"
cmp "$TMP_DIR/p1_eprint.out" "$TMP_DIR/p1_eprint.out.expected"

# M13.1-P1: fs_list — directory entry names (excluding . and ..), strcmp-
# sorted, joined with \n, no trailing newline; missing/unreadable dir
# yields the empty string (LANGUAGE_SPEC §19.6).
FS_LIST_DIR="$TMP_DIR/p1_fs_list_dir"
mkdir -p "$FS_LIST_DIR/sub"
: > "$FS_LIST_DIR/b.txt"
: > "$FS_LIST_DIR/a.txt"
: > "$FS_LIST_DIR/c"
cat > "$TMP_DIR/p1_fs_list.tiq" <<'EOF'
print(fs_list(cli_arg(0)))
print(len(fs_list(cli_arg(1))))
EOF
./build/tiq build "$TMP_DIR/p1_fs_list.tiq" -o "$TMP_DIR/p1_fs_list"
"$TMP_DIR/p1_fs_list" "$FS_LIST_DIR" "$TMP_DIR/p1_no_such_dir" > "$TMP_DIR/p1_fs_list.out"
printf 'a.txt\nb.txt\nc\nsub\n0\n' > "$TMP_DIR/p1_fs_list.expected"
cmp "$TMP_DIR/p1_fs_list.out" "$TMP_DIR/p1_fs_list.expected"

# M13.1-P1: str_sub and fs_list results are heap-owned and freed at scope
# end in reverse declaration order, like the m92 goldens (LANGUAGE_SPEC §16.4).
mkdir -p "$TMP_DIR/p1_empty_dir"
cat > "$TMP_DIR/p1_owned.tiq" <<'EOF'
a = str_sub("hello", 1, 4)
b = fs_list(cli_arg(0))
print(a)
print(b)
EOF
./build/tiq emit-c "$TMP_DIR/p1_owned.tiq" > "$TMP_DIR/p1_owned.c"
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/p1_owned.c" > "$TMP_DIR/p1_owned.frees" || true
printf 'free((void *)b);\nfree((void *)a);\n' > "$TMP_DIR/p1_owned.frees.expected"
cmp "$TMP_DIR/p1_owned.frees" "$TMP_DIR/p1_owned.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/p1_owned_asan" "$TMP_DIR/p1_owned.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/p1_owned_asan" "$TMP_DIR/p1_empty_dir" > "$TMP_DIR/p1_owned.out"
printf 'ell\n\n' > "$TMP_DIR/p1_owned.expected"
cmp "$TMP_DIR/p1_owned.out" "$TMP_DIR/p1_owned.expected"

# M13.1-P2: enums end-to-end — declaration, Name.Variant in bindings,
# comparisons, match scrutinee/patterns, and function arguments; variants
# are plain i64 values numbered 0..n-1 (LANGUAGE_SPEC §17.5). Output pinned.
cat > "$TMP_DIR/p2_enum.tiq" <<'EOF'
enum Color { Red, Green, Blue }
enum Status { Ok, Err }
print(Color.Red)
print(Color.Green)
print(Color.Blue)
print(Status.Err)
c = Color.Blue
print(c == Color.Blue ? 1 : 0)
print(c == Color.Red ? 1 : 0)
name = match c { Color.Red => "red", Color.Green => "green", Color.Blue => "blue", _ => "?" }
print(name)
pick s -> s == Status.Ok ? 100 : 200
print(pick(Status.Ok))
print(pick(Status.Err))
EOF
./build/tiq build "$TMP_DIR/p2_enum.tiq" -o "$TMP_DIR/p2_enum"
"$TMP_DIR/p2_enum" > "$TMP_DIR/p2_enum.out"
printf '0\n1\n2\n1\n1\n0\nblue\n100\n200\n' > "$TMP_DIR/p2_enum.expected"
cmp "$TMP_DIR/p2_enum.out" "$TMP_DIR/p2_enum.expected"

# The emitted C names each variant as a deterministic, readable enum
# constant tiq_enum_<Name>_<Variant>, and use sites emit the constant name.
./build/tiq emit-c "$TMP_DIR/p2_enum.tiq" > "$TMP_DIR/p2_enum.c"
grep -q 'tiq_enum_Color_Red = 0' "$TMP_DIR/p2_enum.c"
grep -q 'tiq_enum_Color_Blue = 2' "$TMP_DIR/p2_enum.c"
grep -q 'tiq_enum_Status_Err = 1' "$TMP_DIR/p2_enum.c"

# M13.1-P3: int vec end-to-end — growth past the initial capacity of 8,
# push/get/set/len/pop (LANGUAGE_SPEC §19.7). Output pinned.
cat > "$TMP_DIR/p3_vec_int.tiq" <<'EOF'
v = vec_new()
[i <- 0..12] { vec_push(v, i * 2) }
print(vec_len(v))
print(vec_get(v, 0))
print(vec_get(v, 11))
vec_set(v, 5, 100)
print(vec_get(v, 5))
print(vec_pop(v))
print(vec_len(v))
EOF
./build/tiq build "$TMP_DIR/p3_vec_int.tiq" -o "$TMP_DIR/p3_vec_int"
"$TMP_DIR/p3_vec_int" > "$TMP_DIR/p3_vec_int.out"
printf '12\n0\n22\n100\n22\n11\n' > "$TMP_DIR/p3_vec_int.expected"
cmp "$TMP_DIR/p3_vec_int.out" "$TMP_DIR/p3_vec_int.expected"

# M13.1-P3: str vec — elements are copied on push (§19.7). Output pinned.
cat > "$TMP_DIR/p3_vec_str.tiq" <<'EOF'
v = vec_new()
vec_push(v, "alpha")
vec_push(v, "beta")
vec_set(v, 0, "gamma")
print(vec_get(v, 0))
print(vec_get(v, 1))
print(vec_pop(v))
print(vec_len(v))
EOF
./build/tiq build "$TMP_DIR/p3_vec_str.tiq" -o "$TMP_DIR/p3_vec_str"
"$TMP_DIR/p3_vec_str" > "$TMP_DIR/p3_vec_str.out"
printf 'gamma\nbeta\nbeta\n1\n' > "$TMP_DIR/p3_vec_str.expected"
cmp "$TMP_DIR/p3_vec_str.out" "$TMP_DIR/p3_vec_str.expected"

# M13.1-P3: struct vec — elements copied in and out by value (§19.7).
cat > "$TMP_DIR/p3_vec_struct.tiq" <<'EOF'
struct Point {
  x: i64,
  y: i64
}
v = vec_new()
p = Point { x: 3, y: 4 }
vec_push(v, p)
vec_push(v, Point { x: 7, y: 9 })
g = vec_get(v, 1)
print(g.x)
print(g.y)
h = vec_pop(v)
print(h.y)
print(vec_len(v))
print(vec_get(v, 0).x)
EOF
./build/tiq build "$TMP_DIR/p3_vec_struct.tiq" -o "$TMP_DIR/p3_vec_struct"
"$TMP_DIR/p3_vec_struct" > "$TMP_DIR/p3_vec_struct.out"
printf '7\n9\n9\n1\n3\n' > "$TMP_DIR/p3_vec_struct.expected"
cmp "$TMP_DIR/p3_vec_struct.out" "$TMP_DIR/p3_vec_struct.expected"

# M13.1-P3: out-of-range vec_get aborts deterministically — exact stderr
# message and exit code 1, never UB (§19.7).
cat > "$TMP_DIR/p3_vec_oob.tiq" <<'EOF'
v = vec_new()
vec_push(v, 5)
print(vec_get(v, 3))
EOF
./build/tiq build "$TMP_DIR/p3_vec_oob.tiq" -o "$TMP_DIR/p3_vec_oob"
oob_status=0
"$TMP_DIR/p3_vec_oob" > "$TMP_DIR/p3_vec_oob.out" 2> "$TMP_DIR/p3_vec_oob.err" || oob_status=$?
[ "$oob_status" -eq 1 ]
printf 'tiq: vec index 3 out of bounds for vec of length 1\n' > "$TMP_DIR/p3_vec_oob.err.expected"
cmp "$TMP_DIR/p3_vec_oob.err" "$TMP_DIR/p3_vec_oob.err.expected"
[ ! -s "$TMP_DIR/p3_vec_oob.out" ]

# M13.1-P3: vec_pop on an empty vec aborts deterministically (§19.7).
cat > "$TMP_DIR/p3_vec_pop_empty.tiq" <<'EOF'
v = vec_new()
vec_push(v, 1)
x = vec_pop(v)
y = vec_pop(v)
EOF
./build/tiq build "$TMP_DIR/p3_vec_pop_empty.tiq" -o "$TMP_DIR/p3_vec_pop_empty"
pop_status=0
"$TMP_DIR/p3_vec_pop_empty" > /dev/null 2> "$TMP_DIR/p3_vec_pop_empty.err" || pop_status=$?
[ "$pop_status" -eq 1 ]
printf 'tiq: vec_pop on empty vec\n' > "$TMP_DIR/p3_vec_pop_empty.err.expected"
cmp "$TMP_DIR/p3_vec_pop_empty.err" "$TMP_DIR/p3_vec_pop_empty.err.expected"

# M13.1-P4: StrBuf end-to-end — append/len/to_str; to_str is a snapshot,
# later appends do not change it (LANGUAGE_SPEC §19.8). Output pinned.
cat > "$TMP_DIR/p4_strbuf.tiq" <<'EOF'
sb = str_buf_new()
print(str_buf_len(sb))
n = str_buf_append(sb, "Hello")
print(n)
str_buf_append(sb, ", ")
str_buf_append(sb, "World")
str_buf_append(sb, "")
s = str_buf_to_str(sb)
print(s)
print(str_buf_len(sb))
str_buf_append(sb, "!")
t = str_buf_to_str(sb)
print(t)
print(s)
print(len(t))
EOF
./build/tiq build "$TMP_DIR/p4_strbuf.tiq" -o "$TMP_DIR/p4_strbuf"
"$TMP_DIR/p4_strbuf" > "$TMP_DIR/p4_strbuf.out"
printf '0\n5\nHello, World\n12\nHello, World!\nHello, World\n13\n' > "$TMP_DIR/p4_strbuf.expected"
cmp "$TMP_DIR/p4_strbuf.out" "$TMP_DIR/p4_strbuf.expected"

# M13.1-P4: stress — 1000 appends of a 16-byte chunk accumulate 16000 bytes
# (>= 10 KB) through the doubling growth path; exact length and boundary
# substrings pinned. This is the O(n) guarantee the Tiq emitter port (S4)
# relies on; an O(n²) concat regression would time this out (§19.8).
cat > "$TMP_DIR/p4_strbuf_stress.tiq" <<'EOF'
sb = str_buf_new()
[i <- 0..1000] { str_buf_append(sb, "0123456789ABCDEF") }
print(str_buf_len(sb))
s = str_buf_to_str(sb)
print(len(s))
print(str_sub(s, 0, 16))
print(str_sub(s, 15984, 16000))
EOF
./build/tiq build "$TMP_DIR/p4_strbuf_stress.tiq" -o "$TMP_DIR/p4_strbuf_stress"
"$TMP_DIR/p4_strbuf_stress" > "$TMP_DIR/p4_strbuf_stress.out"
printf '16000\n16000\n0123456789ABCDEF\n0123456789ABCDEF\n' > "$TMP_DIR/p4_strbuf_stress.expected"
cmp "$TMP_DIR/p4_strbuf_stress.out" "$TMP_DIR/p4_strbuf_stress.expected"

# M13.1-P4: str_buf_to_str results are heap-owned and freed at scope end in
# reverse declaration order, like str_sub in the p1_owned golden (§16.4);
# the strbuf handle itself is never freed (leak-never-dangle, §19.8).
cat > "$TMP_DIR/p4_owned.tiq" <<'EOF'
sb = str_buf_new()
str_buf_append(sb, "abc")
a = str_buf_to_str(sb)
b = str_sub("hello", 0, 2)
print(a)
print(b)
EOF
./build/tiq emit-c "$TMP_DIR/p4_owned.tiq" > "$TMP_DIR/p4_owned.c"
grep -o 'free((void \*)[a-z]*);' "$TMP_DIR/p4_owned.c" > "$TMP_DIR/p4_owned.frees" || true
printf 'free((void *)b);\nfree((void *)a);\n' > "$TMP_DIR/p4_owned.frees.expected"
cmp "$TMP_DIR/p4_owned.frees" "$TMP_DIR/p4_owned.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/p4_owned_asan" "$TMP_DIR/p4_owned.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/p4_owned_asan" > "$TMP_DIR/p4_owned.out"
printf 'abc\nhe\n' > "$TMP_DIR/p4_owned.expected"
cmp "$TMP_DIR/p4_owned.out" "$TMP_DIR/p4_owned.expected"

# M13.1-P5: Map end-to-end — set/get/has/len, overwrite keeps the length,
# updates the value, and keeps the insertion position; a missing key yields
# -1 and map_has false (LANGUAGE_SPEC §19.9). Output pinned.
cat > "$TMP_DIR/p5_map.tiq" <<'EOF'
m = map_new()
print(map_len(m))
n = map_set(m, "alpha", 1)
print(n)
map_set(m, "beta", 2)
print(map_get(m, "alpha"))
print(map_get(m, "beta"))
print(map_has(m, "alpha"))
print(map_has(m, "gamma"))
print(map_get(m, "gamma"))
map_set(m, "alpha", 42)
print(map_len(m))
print(map_get(m, "alpha"))
print(map_key_at(m, 0))
print(map_key_at(m, 1))
print(map_val_at(m, 0))
print(map_has(m, ""))
map_set(m, "", 7)
print(map_get(m, ""))
print(map_len(m))
EOF
./build/tiq build "$TMP_DIR/p5_map.tiq" -o "$TMP_DIR/p5_map"
"$TMP_DIR/p5_map" > "$TMP_DIR/p5_map.out"
printf '0\n1\n1\n2\ntrue\nfalse\n-1\n2\n42\nalpha\nbeta\n42\nfalse\n7\n3\n' > "$TMP_DIR/p5_map.expected"
cmp "$TMP_DIR/p5_map.out" "$TMP_DIR/p5_map.expected"

# M13.1-P5: determinism — 12 keys cross the 0.7 load factor twice (8 -> 16
# -> 32 buckets), a mid-stream overwrite must not move its key, and
# map_key_at/map_val_at iteration must reproduce insertion order exactly.
# Golden pinned AND the binary is run twice with byte-identical output
# (iteration never depends on bucket layout, §19.9).
cat > "$TMP_DIR/p5_map_iter.tiq" <<'EOF'
m = map_new()
map_set(m, "one", 1)
map_set(m, "two", 2)
map_set(m, "three", 3)
map_set(m, "four", 4)
map_set(m, "five", 5)
map_set(m, "six", 6)
map_set(m, "seven", 7)
map_set(m, "three", 33)
map_set(m, "eight", 8)
map_set(m, "nine", 9)
map_set(m, "ten", 10)
map_set(m, "eleven", 11)
map_set(m, "twelve", 12)
n = map_len(m)
print(n)
[i <- 0..n] {
    print(map_key_at(m, i))
    print(map_val_at(m, i))
}
EOF
./build/tiq build "$TMP_DIR/p5_map_iter.tiq" -o "$TMP_DIR/p5_map_iter"
"$TMP_DIR/p5_map_iter" > "$TMP_DIR/p5_map_iter.out"
printf '12\none\n1\ntwo\n2\nthree\n33\nfour\n4\nfive\n5\nsix\n6\nseven\n7\neight\n8\nnine\n9\nten\n10\neleven\n11\ntwelve\n12\n' > "$TMP_DIR/p5_map_iter.expected"
cmp "$TMP_DIR/p5_map_iter.out" "$TMP_DIR/p5_map_iter.expected"
"$TMP_DIR/p5_map_iter" > "$TMP_DIR/p5_map_iter.out2"
cmp "$TMP_DIR/p5_map_iter.out" "$TMP_DIR/p5_map_iter.out2"

# M13.1-P5: map_key_at out of range aborts deterministically (§19.9).
cat > "$TMP_DIR/p5_map_oob.tiq" <<'EOF'
m = map_new()
map_set(m, "a", 1)
k = map_key_at(m, 1)
print(k)
EOF
./build/tiq build "$TMP_DIR/p5_map_oob.tiq" -o "$TMP_DIR/p5_map_oob"
map_oob_status=0
"$TMP_DIR/p5_map_oob" > "$TMP_DIR/p5_map_oob.out" 2> "$TMP_DIR/p5_map_oob.err" || map_oob_status=$?
[ "$map_oob_status" -eq 1 ]
printf 'tiq: map index 1 out of bounds for map of length 1\n' > "$TMP_DIR/p5_map_oob.err.expected"
cmp "$TMP_DIR/p5_map_oob.err" "$TMP_DIR/p5_map_oob.err.expected"
[ ! -s "$TMP_DIR/p5_map_oob.out" ]

# M13.1-P5: map_val_at with a negative index aborts the same way (§19.9).
cat > "$TMP_DIR/p5_map_oob_neg.tiq" <<'EOF'
m = map_new()
map_set(m, "a", 1)
v = map_val_at(m, 0 - 1)
print(v)
EOF
./build/tiq build "$TMP_DIR/p5_map_oob_neg.tiq" -o "$TMP_DIR/p5_map_oob_neg"
map_neg_status=0
"$TMP_DIR/p5_map_oob_neg" > /dev/null 2> "$TMP_DIR/p5_map_oob_neg.err" || map_neg_status=$?
[ "$map_neg_status" -eq 1 ]
printf 'tiq: map index -1 out of bounds for map of length 1\n' > "$TMP_DIR/p5_map_oob_neg.err.expected"
cmp "$TMP_DIR/p5_map_oob_neg.err" "$TMP_DIR/p5_map_oob_neg.err.expected"

# M13.1-P8 item 1: s[i] byte indexing on str and str views compiles and
# returns the raw byte as int (§13.1); previously the emitter fell through
# to stream-generator emission and produced invalid C. Output pinned:
# 97+98+99 = 294, then s[0] = 97, then view v = s[1..] has v[0] = 98.
cat > "$TMP_DIR/p8_str_index.tiq" <<'EOF'
s = "abc"
total <- 0
[i <- 0..3] {
    total += s[i]
}
print(total)
print(s[0])
v = s[1..]
print(v[0])
EOF
./build/tiq build "$TMP_DIR/p8_str_index.tiq" -o "$TMP_DIR/p8_str_index"
"$TMP_DIR/p8_str_index" > "$TMP_DIR/p8_str_index.out"
printf '294\n97\n98\n' > "$TMP_DIR/p8_str_index.expected"
cmp "$TMP_DIR/p8_str_index.out" "$TMP_DIR/p8_str_index.expected"

# M13.1-P8 item 1: out-of-range s[i] aborts deterministically — exact
# stderr message and exit code 1, matching the array precedent (§13.1).
cat > "$TMP_DIR/p8_str_index_oob.tiq" <<'EOF'
s = "abc"
print(s[5])
EOF
./build/tiq build "$TMP_DIR/p8_str_index_oob.tiq" -o "$TMP_DIR/p8_str_index_oob"
str_oob_status=0
"$TMP_DIR/p8_str_index_oob" > "$TMP_DIR/p8_str_index_oob.out" 2> "$TMP_DIR/p8_str_index_oob.err" || str_oob_status=$?
[ "$str_oob_status" -eq 1 ]
printf 'tiq: index 5 out of bounds for string of length 3\n' > "$TMP_DIR/p8_str_index_oob.err.expected"
cmp "$TMP_DIR/p8_str_index_oob.err" "$TMP_DIR/p8_str_index_oob.err.expected"
[ ! -s "$TMP_DIR/p8_str_index_oob.out" ]

# M13.1-P8 item 2: containers cross function boundaries (§19.10). A vec
# argument shares its handle (callee pushes visible to the caller), an
# unestablished vec is established by the annotated parameter, and a
# vec[int] return carries the element type to the caller. Output pinned.
cat > "$TMP_DIR/p8_vec_across_fns.tiq" <<'EOF'
fill v:vec[int] -> int -> {
  vec_push(v, 10)
  vec_push(v, 20)
  vec_len(v)
}
mk seed:int -> vec[int] -> {
  v = vec_new()
  vec_push(v, seed)
  v
}
v = vec_new()
n = fill(v)
print(n)
print(vec_get(v, 1))
w = mk(7)
print(vec_get(w, 0))
EOF
./build/tiq build "$TMP_DIR/p8_vec_across_fns.tiq" -o "$TMP_DIR/p8_vec_across_fns"
"$TMP_DIR/p8_vec_across_fns" > "$TMP_DIR/p8_vec_across_fns.out"
printf '2\n20\n7\n' > "$TMP_DIR/p8_vec_across_fns.expected"
cmp "$TMP_DIR/p8_vec_across_fns.out" "$TMP_DIR/p8_vec_across_fns.expected"

# M13.1-P8 item 2: a vec of named structs passes between functions with
# its nominal element type intact (§19.10).
cat > "$TMP_DIR/p8_vec_struct_across.tiq" <<'EOF'
struct Tok { kind: int, val: int }
first v:vec[Tok] -> Tok -> vec_get(v, 0)
v = vec_new()
vec_push(v, Tok { kind: 2, val: 5 })
t = first(v)
print(t.kind)
EOF
./build/tiq build "$TMP_DIR/p8_vec_struct_across.tiq" -o "$TMP_DIR/p8_vec_struct_across"
"$TMP_DIR/p8_vec_struct_across" > "$TMP_DIR/p8_vec_struct_across.out"
printf '2\n' > "$TMP_DIR/p8_vec_struct_across.expected"
cmp "$TMP_DIR/p8_vec_struct_across.out" "$TMP_DIR/p8_vec_struct_across.expected"

# M13.1-P8 item 2: a strbuf handle passed to a function is appended
# through the shared handle; the caller snapshots the result (§19.10).
cat > "$TMP_DIR/p8_strbuf_across.tiq" <<'EOF'
addtwice sb:strbuf s:str -> int -> {
  str_buf_append(sb, s)
  str_buf_append(sb, s)
  str_buf_len(sb)
}
sb = str_buf_new()
n = addtwice(sb, "hi")
print(n)
out = str_buf_to_str(sb)
print(out)
EOF
./build/tiq build "$TMP_DIR/p8_strbuf_across.tiq" -o "$TMP_DIR/p8_strbuf_across"
"$TMP_DIR/p8_strbuf_across" > "$TMP_DIR/p8_strbuf_across.out"
printf '4\nhihi\n' > "$TMP_DIR/p8_strbuf_across.expected"
cmp "$TMP_DIR/p8_strbuf_across.out" "$TMP_DIR/p8_strbuf_across.expected"

# M13.1-P8 item 2: a map set in the callee is visible in the caller —
# shared-handle semantics, not a copy (§19.10).
cat > "$TMP_DIR/p8_map_across.tiq" <<'EOF'
put m:map -> int -> map_set(m, "answer", 42)
m = map_new()
put(m)
print(map_get(m, "answer"))
EOF
./build/tiq build "$TMP_DIR/p8_map_across.tiq" -o "$TMP_DIR/p8_map_across"
"$TMP_DIR/p8_map_across" > "$TMP_DIR/p8_map_across.out"
printf '42\n' > "$TMP_DIR/p8_map_across.expected"
cmp "$TMP_DIR/p8_map_across.out" "$TMP_DIR/p8_map_across.expected"

# M13.1-P9: a vec filled from user-function returns crosses an annotated
# vec[int] parameter boundary (was a false E09 "expected vec<int>, found
# vec<int>" — element types compared by pooled-pointer identity while
# type_get_func interns function return types per-arity). Output pinned:
# f(1)=2, f(2)=3, so sum(v)=5 and vec_get(v,1)+1 = 4.
cat > "$TMP_DIR/p9_vec_helper_fill.tiq" <<'EOF'
f x:int -> int -> x + 1
sum v:vec[int] -> int -> {
  total <- 0
  n = vec_len(v)
  [i <- 0..n] { total += vec_get(v, i) }
  total
}
v = vec_new()
vec_push(v, f(1))
vec_push(v, f(2))
print(sum(v))
print(vec_get(v, 1) + 1)
EOF
./build/tiq build "$TMP_DIR/p9_vec_helper_fill.tiq" -o "$TMP_DIR/p9_vec_helper_fill"
"$TMP_DIR/p9_vec_helper_fill" > "$TMP_DIR/p9_vec_helper_fill.out"
printf '5\n4\n' > "$TMP_DIR/p9_vec_helper_fill.expected"
cmp "$TMP_DIR/p9_vec_helper_fill.out" "$TMP_DIR/p9_vec_helper_fill.expected"

echo "smoke: ok"
