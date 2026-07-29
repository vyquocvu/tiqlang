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

# net_fetch("ftp://...") fails the scheme check without touching the network
# (LANGUAGE_SPEC §19.2), keeping this test deterministic.
printf 'cmd = "true"\nres = proc_exec(cmd)\nval = json_parse_int("42")\nenc = json_encode_str("test")\nnet = net_fetch("ftp://unused")\n' > "$TMP_DIR/m6_sys.tiq"
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

# M8: Propagation operator (expr?).
printf 'x = some(42)\ny = x?\nprint(y)\n' > "$TMP_DIR/propagate_some.tiq"
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

# M9.2: owned heap strings are freed at scope end, reverse declaration order,
# after defers (LANGUAGE_SPEC §16.4); aliases and cli_arg results are not
# freed. ASan build of the emitted C catches double-free / use-after-free.
cat > "$TMP_DIR/m92_scope.tiq" <<'EOF'
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
[0..2] {
    f = json_get("{\"n\": 7}", "n")
    print(f)
}
EOF
./build/tiq emit-c "$TMP_DIR/m92_scope.tiq" > "$TMP_DIR/m92_scope.c"
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/m92_scope.c" > "$TMP_DIR/m92_scope.frees" || true
printf 'free((void *)e);\nfree((void *)f);\nfree((void *)b);\nfree((void *)a);\n' > "$TMP_DIR/m92_scope.frees.expected"
cmp "$TMP_DIR/m92_scope.frees" "$TMP_DIR/m92_scope.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92_scope_asan" "$TMP_DIR/m92_scope.c"
"$TMP_DIR/m92_scope_asan" > "$TMP_DIR/m92_scope.out"
printf 'one\n"two"\none\n\n1\nv\n7\n7\n' > "$TMP_DIR/m92_scope.expected"
cmp "$TMP_DIR/m92_scope.out" "$TMP_DIR/m92_scope.expected"

# M9.2-B: break/skip destroy the owned strings of every scope they exit
# (innermost first, through the enclosing loop body) before jumping; only
# owners already bound at the jump point are freed (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92b_early.tiq" <<'EOF'
[0..3] {
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
[0..2] {
    f = json_get("{\"k\": 3}", "k")
    print(f)
    skip
    print(999)
}
[0..2] {
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
"$TMP_DIR/m92b_early_asan" > "$TMP_DIR/m92b_early.out"
printf '5\n7\n3\n3\n2\n' > "$TMP_DIR/m92b_early.expected"
cmp "$TMP_DIR/m92b_early.out" "$TMP_DIR/m92b_early.expected"

# M9.2-C: a scalar-result function frees its body's owned strings after the
# result is computed; a str-result function must not free (the result may
# alias an owner) (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92c_fn.tiq" <<'EOF'
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
"$TMP_DIR/m92c_fn_asan" > "$TMP_DIR/m92c_fn.out"
printf '42\nok\n' > "$TMP_DIR/m92c_fn.expected"
cmp "$TMP_DIR/m92c_fn.out" "$TMP_DIR/m92c_fn.expected"

# M9.2-D: a mutable owner (all assignments are owned-builtin calls, name only
# used as builtin argument) frees its previous string on reassignment and is
# freed at scope end; an aliased mutable is disqualified and never freed
# (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92d_mut.tiq" <<'EOF'
s <- json_get("{\"a\": \"one\"}", "a")
print(s)
s <- json_get("{\"b\": \"two\"}", "b")
print(s)
[0..2] {
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
"$TMP_DIR/m92d_mut_asan" > "$TMP_DIR/m92d_mut.out"
printf 'one\ntwo\nthree\nthree\nfour\n' > "$TMP_DIR/m92d_mut.expected"
cmp "$TMP_DIR/m92d_mut.out" "$TMP_DIR/m92d_mut.expected"

# M9.2-E: a statement-level proc_exit destroys the owned strings of every
# enclosing scope (innermost first) before terminating; the exit code is
# computed before destruction (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92e_exit.tiq" <<'EOF'
a = json_get("{\"a\": \"one\"}", "a")
print(a)
[0..3] {
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
"$TMP_DIR/m92e_exit_asan" > "$TMP_DIR/m92e_exit.out" || exit_status=$?
test "$exit_status" -eq 7
printf 'one\ntwo\ntwo\ntwo\n' > "$TMP_DIR/m92e_exit.expected"
cmp "$TMP_DIR/m92e_exit.out" "$TMP_DIR/m92e_exit.expected"

# M9.2-F: an unbound owned-builtin temporary in an unconditionally evaluated
# position of a simple statement (bare statement expression, or argument to a
# standard-library builtin) is hoisted into a hidden binding and freed at the
# end of the statement; a user-function argument is not (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92f_tmp.tiq" <<'EOF'
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
"$TMP_DIR/m92f_tmp_asan" > "$TMP_DIR/m92f_tmp.out"
printf 'one\n7\nthree\n' > "$TMP_DIR/m92f_tmp.expected"
cmp "$TMP_DIR/m92f_tmp.out" "$TMP_DIR/m92f_tmp.expected"

# M9.2-G: a str-result function whose final expression is a string literal or
# a direct owned-builtin call cannot alias a body owner, so it frees its
# owners after computing the result; an identifier result still must not free
# (it may alias an owner) (LANGUAGE_SPEC §16.4).
cat > "$TMP_DIR/m92g_strfn.tiq" <<'EOF'
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
printf 'free((void *)v);\nfree((void *)w);\n' > "$TMP_DIR/m92g_strfn.frees.expected"
cmp "$TMP_DIR/m92g_strfn.frees" "$TMP_DIR/m92g_strfn.frees.expected"
cc -std=c11 -g -fsanitize=address,undefined -o "$TMP_DIR/m92g_strfn_asan" "$TMP_DIR/m92g_strfn.c"
"$TMP_DIR/m92g_strfn_asan" > "$TMP_DIR/m92g_strfn.out"
printf '"hi"\nlit\nok\n' > "$TMP_DIR/m92g_strfn.expected"
cmp "$TMP_DIR/m92g_strfn.out" "$TMP_DIR/m92g_strfn.expected"

echo "smoke: ok"
