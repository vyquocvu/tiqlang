#!/bin/sh
# M13.5-P0: executable differential harness for the first self-hosted C11
# emitter slice. M13.6 owns byte identity with the reference C emitter; this
# package pins deterministic self-host output and observable program behavior.
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
SELFHOST="build/tiq-emit-c-selfhost"
TMP_DIR="${TMPDIR:-/tmp}/tiq-selfhost-emit-c-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/emit_c_main.tiq -o "$SELFHOST" 2>"$TMP_DIR/build.err"; then
  echo "selfhost_emit_c: FAIL (cannot build src/tiq/emit_c_main.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0
count=0

if ! OUT="$TMP_DIR/emit_c_runtime.tiq" tools/gen_selfhost_runtime.sh ||
   ! cmp -s src/tiq/emit_c_runtime.tiq "$TMP_DIR/emit_c_runtime.tiq"; then
  echo "selfhost_emit_c: FAIL generated runtime is stale" >&2
  fail=1
fi

case_run() {
  name="$1"
  source="$2"
  count=$((count + 1))
  printf '%s\n' "$source" >"$TMP_DIR/$name.tiq"

  "$TIQ" run "$TMP_DIR/$name.tiq" >"$TMP_DIR/$name.ref.out" 2>"$TMP_DIR/$name.ref.err"
  ref_rc=$?
  "$SELFHOST" "$TMP_DIR/$name.tiq" >"$TMP_DIR/$name.1.c" 2>"$TMP_DIR/$name.emit.err"
  emit_rc=$?
  "$SELFHOST" "$TMP_DIR/$name.tiq" >"$TMP_DIR/$name.2.c" 2>>"$TMP_DIR/$name.emit.err"
  if [ "$emit_rc" -ne 0 ] || ! cmp -s "$TMP_DIR/$name.1.c" "$TMP_DIR/$name.2.c"; then
    echo "selfhost_emit_c: FAIL $name (emission failed or was nondeterministic)" >&2
    fail=1
    return
  fi
  if ! "$CC_BIN" -std=c11 -Wall -Wextra -Wpedantic -Werror "$TMP_DIR/$name.1.c" -o "$TMP_DIR/$name.bin" 2>"$TMP_DIR/$name.cc.err"; then
    echo "selfhost_emit_c: FAIL $name (generated C did not compile)" >&2
    cat "$TMP_DIR/$name.cc.err" >&2
    fail=1
    return
  fi
  "$TMP_DIR/$name.bin" >"$TMP_DIR/$name.got.out" 2>"$TMP_DIR/$name.got.err"
  got_rc=$?
  if [ "$ref_rc" -ne "$got_rc" ] || ! cmp -s "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" || ! cmp -s "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err"; then
    echo "selfhost_emit_c: FAIL $name (observable behavior mismatch)" >&2
    diff "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" >&2 || true
    diff "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err" >&2 || true
    fail=1
  fi
}

case_run "string" 'msg = "hello"
print(msg)'
case_run "arithmetic" 'x = (7 + 5) * 3 - 4 / 2
print(x)'
case_run "conditional" 'x = 9
print(x > 4 ? 11 : 22)'
case_run "single_branch_conditional" 'x <- 0
?[3 < 5] { x <- 42 }
print(x)'
case_run "bool" 'print(3 < 5 && true)'
case_run "range_loop" 'sum <- 0
[j <- 0..6] { sum += j }
print(sum)'
case_run "while_loop" 'n <- 0
[n < 4] { n += 1 }
print(n)'
case_run "function" 'add a b -> a + b
print(add(20, 22))'
case_run "array_read_len" 'xs = [4, 8, 15, 16]
print(xs[2])
print(len(xs))'
case_run "array_fill" 'xs = [7; 3]
print(xs[0] + xs[1] + xs[2])'
case_run "array_assign" 'xs <- [1, 2, 3]
xs[1] <- 9
xs[2] += 4
print(xs[0] + xs[1] + xs[2])'
case_run "array_oob_read" 'xs = [1, 2]
print(xs[2])'
case_run "array_oob_write" 'xs <- [1, 2]
xs[3] <- 9'
case_run "slice_basic" 'xs = [10, 20, 30, 40, 50]
s = xs[1..3]
print(s[0])
print(s[1])'
case_run "slice_open_end" 'xs = [1, 2, 3, 4]
s = xs[2..]
print(s[0])
print(s[1])'
case_run "slice_len" 'xs = [5, 6, 7, 8, 9]
s = xs[1..4]
print(len(s))'
case_run "slice_of_slice" 'xs = [10, 20, 30, 40, 50]
s = xs[1..4]
t = s[1..2]
print(t[0])
print(len(t))'
case_run "slice_open_start" 'xs = [7, 8, 9, 10]
s = xs[..2]
print(s[0])
print(s[1])
print(len(s))'
case_run "struct_record_field" 'struct Point { x: i64, y: i64 }
p = Point { x: 20, y: 22 }
print(p.x + p.y)'
case_run "enum_variant" 'enum Color { Red, Green, Blue }
c = Color.Blue
print(c)'
case_run "match_wildcard" 'x = 2
print(match x { 1 => 10, 2 => 20, _ => 30 })'
case_run "option_result" 'a = some(7)
b = none
r = ok(9)
print(a ?? 3)
print(b ?? 4)
print(r ?? 5)
print(a?)
print(r?)'
case_run "numeric_conversions" 'print(i8(130))
print(u16(65537))
print(i32(42.9))
print(f32(7))'
case_run "owned_scopes" 'a = json_get("{\"x\":\"one\"}", "x")
{
  b = json_encode_str("two")
  defer print(b)
}
[0..2] {
  c = str_cat("c", "d")
  print(c)
}
print(a)'
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/owned_scopes.1.c" >"$TMP_DIR/owned_scopes.frees" || true
printf 'free((void *)b);\nfree((void *)c);\nfree((void *)a);\n' >"$TMP_DIR/owned_scopes.frees.expected"
if ! cmp -s "$TMP_DIR/owned_scopes.frees" "$TMP_DIR/owned_scopes.frees.expected"; then
  echo "selfhost_emit_c: FAIL owned_scopes (destruction order mismatch)" >&2
  fail=1
fi
if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/owned_scopes.1.c" -o "$TMP_DIR/owned_scopes.asan" 2>/dev/null; then
  ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/owned_scopes.asan" >/dev/null 2>"$TMP_DIR/owned_scopes.asan.err" || {
    echo "selfhost_emit_c: FAIL owned_scopes (sanitizer runtime)" >&2
    cat "$TMP_DIR/owned_scopes.asan.err" >&2
    fail=1
  }
fi
case_run "owned_early_exit" '[0..2] {
  a = json_get("{\"a\":1}", "a")
  [(i < 1)] {
    b = json_get("{\"b\":2}", "b")
    print(b)
    break
  }
  print(a)
  break
  d = json_get("{\"d\":3}", "d")
}
[0..2] {
  f = json_get("{\"f\":4}", "f")
  print(f)
  skip
}'
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/owned_early_exit.1.c" >"$TMP_DIR/owned_early_exit.frees" || true
printf 'free((void *)b);\nfree((void *)b);\nfree((void *)a);\nfree((void *)d);\nfree((void *)a);\nfree((void *)f);\nfree((void *)f);\n' >"$TMP_DIR/owned_early_exit.frees.expected"
if ! cmp -s "$TMP_DIR/owned_early_exit.frees" "$TMP_DIR/owned_early_exit.frees.expected"; then
  echo "selfhost_emit_c: FAIL owned_early_exit (destruction order mismatch)" >&2
  cat "$TMP_DIR/owned_early_exit.frees" >&2
  fail=1
fi
case_run "owned_mutable" 's <- json_get("{\"a\":\"one\"}", "a")
print(s)
s <- json_get("{\"b\":\"two\"}", "b")
print(s)
[0..2] {
  s <- json_get("{\"c\":\"three\"}", "c")
  print(s)
}
t <- json_get("{\"d\":\"four\"}", "d")
u = t
print(u)'
grep -o 'free((void \*)[a-z_]*);' "$TMP_DIR/owned_mutable.1.c" >"$TMP_DIR/owned_mutable.frees" || true
printf 'free((void *)tiq_old);\nfree((void *)tiq_old);\nfree((void *)s);\n' >"$TMP_DIR/owned_mutable.frees.expected"
if ! cmp -s "$TMP_DIR/owned_mutable.frees" "$TMP_DIR/owned_mutable.frees.expected"; then
  echo "selfhost_emit_c: FAIL owned_mutable (qualification/destruction mismatch)" >&2
  cat "$TMP_DIR/owned_mutable.frees" >&2
  fail=1
fi
case_run "owned_proc_exit" 'a = json_get("{\"a\":\"one\"}", "a")
print(a)
[0..3] {
  b = json_get("{\"b\":\"two\"}", "b")
  print(b)
  ?[i < 3] { proc_exit(7) }
}'
grep -o 'free((void \*)[a-z]);' "$TMP_DIR/owned_proc_exit.1.c" >"$TMP_DIR/owned_proc_exit.frees" || true
printf 'free((void *)b);\nfree((void *)a);\nfree((void *)b);\nfree((void *)a);\n' >"$TMP_DIR/owned_proc_exit.frees.expected"
if ! cmp -s "$TMP_DIR/owned_proc_exit.frees" "$TMP_DIR/owned_proc_exit.frees.expected"; then
  echo "selfhost_emit_c: FAIL owned_proc_exit (destruction order mismatch)" >&2
  cat "$TMP_DIR/owned_proc_exit.frees" >&2
  fail=1
fi
case_run "owned_fresh_calls" 'mk src:str -> str -> {
  a = json_get(src, "a")
  json_encode_str(a)
}
pick src:str -> str -> {
  b = json_get(src, "b")
  c = json_get(src, "c")
  c
}
lit src:str -> str -> "static"
v = mk("{\"a\":\"one\"}")
u = pick("{\"b\":\"two\",\"c\":\"three\"}")
w = lit("x")
print(v)
print(u)
print(w)'
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/owned_fresh_calls.1.c" >"$TMP_DIR/owned_fresh_calls.frees" || true
printf 'free((void *)u);\nfree((void *)v);\nfree((void *)a);\nfree((void *)b);\n' >"$TMP_DIR/owned_fresh_calls.frees.expected"
if ! cmp -s "$TMP_DIR/owned_fresh_calls.frees" "$TMP_DIR/owned_fresh_calls.frees.expected"; then
  echo "selfhost_emit_c: FAIL owned_fresh_calls (fresh-result ownership mismatch)" >&2
  cat "$TMP_DIR/owned_fresh_calls.frees" >&2
  fail=1
fi
case_run "owned_temporaries" 'mk src:str -> str -> json_encode_str(src)
keep src:str -> str -> src
print(json_get("{\"k\":\"one\"}", "k"))
json_encode_str("two")
print(mk(json_get("{\"k\":\"three\"}", "k")))
print(keep(json_get("{\"k\":\"four\"}", "k")))
print(len(1 > 0 ? mk("five") : json_encode_str("six")))'
grep -o 'free((void \*)[a-z_0-9]*);' "$TMP_DIR/owned_temporaries.1.c" >"$TMP_DIR/owned_temporaries.frees" || true
printf 'free((void *)tiq_tmp0);\nfree((void *)tiq_tmp1);\nfree((void *)tiq_tmp3);\nfree((void *)tiq_tmp2);\nfree((void *)tiq_tmp4);\n' >"$TMP_DIR/owned_temporaries.frees.expected"
if ! cmp -s "$TMP_DIR/owned_temporaries.frees" "$TMP_DIR/owned_temporaries.frees.expected"; then
  echo "selfhost_emit_c: FAIL owned_temporaries (temporary ownership mismatch)" >&2
  cat "$TMP_DIR/owned_temporaries.frees" >&2
  fail=1
fi
case_run "string_view" 'msg = "hello"
mid = msg[1..4]
print(mid)
print(len(mid))'
case_run "string_byte_index" 'msg = "abc"
print(msg[0])
tail = msg[1..]
print(tail[1])'
case_run "string_index_oob" 'msg = "hi"
print(msg[2])'
case_run "stream_fib" 'fib = [0, 1, ... a + b]
print(fib[0])
print(fib[10])'
case_run "stream_single" 'powers = [1, ... x * 2]
print(powers[0])
print(powers[5])'
case_run "stream_function" 'pow b -> [1, ... x * b]
print(pow(2)[0])
print(pow(2)[10])
print(pow(3)[3])'
case_run "defer_reverse" 'f x -> {
  defer print(1)
  defer print(2)
  print(x)
  x + 1
}
print(f(7))'
case_run "borrow_mut" 'bump r:&mut i64 -> {
  r <- r + 1
  r
}
x <- 41
print(bump(&mut x))
print(x)'
case_run "borrow_shared" 'add a:&i64 b:&i64 -> a + b
x = 20
y = 22
print(add(&x, &y))'
case_run "vec_int" 'v = vec_new()
vec_push(v, 10)
vec_push(v, 32)
vec_set(v, 0, 7)
print(vec_len(v))
print(vec_get(v, 0) + vec_pop(v))'
case_run "vec_str" 'v = vec_new()
vec_push(v, "alpha")
vec_push(v, "beta")
print(vec_get(v, 0))
print(vec_pop(v))'
case_run "strbuf" 'b = str_buf_new()
str_buf_append(b, "tiq")
str_buf_append(b, "lang")
print(str_buf_len(b))
print(str_buf_to_str(b))'
case_run "map" 'm = map_new()
map_set(m, "a", 20)
map_set(m, "b", 22)
print(map_len(m))
print(map_has(m, "a"))
print(map_get(m, "a") + map_get(m, "b"))
print(map_get(m, "missing"))
print(map_key_at(m, 1))
print(map_val_at(m, 1))'
case_run "json_runtime" 'print(json_parse_int("42"))
print(json_encode_str("tiq"))
print(json_get("{\"name\":\"Tiq\"}", "name"))
print(json_arr_len("[1,2,3]"))
print(json_arr_get("[10,20]", 1))'

# Module graph coverage: deterministic post-order flattening and normalized
# relative-path dedupe must produce one definition and executable C.
mkdir -p "$TMP_DIR/modules/sub"
printf '%s\n' 'answer x -> 42' >"$TMP_DIR/modules/sub/d.tiq"
printf '%s\n' 'import "sub/d.tiq"' >"$TMP_DIR/modules/b.tiq"
printf '%s\n' 'import "./sub/../sub/d.tiq"' >"$TMP_DIR/modules/c.tiq"
printf '%s\n' 'import "b.tiq"' 'import "c.tiq"' 'print(answer(0))' >"$TMP_DIR/modules/main.tiq"
"$SELFHOST" "$TMP_DIR/modules/main.tiq" >"$TMP_DIR/modules/main.c" 2>"$TMP_DIR/modules/main.err"
if [ "$?" -ne 0 ] || ! "$CC_BIN" -std=c11 -Wall -Wextra -Wpedantic -Werror "$TMP_DIR/modules/main.c" -o "$TMP_DIR/modules/main.bin" ||
   ! "$TMP_DIR/modules/main.bin" >"$TMP_DIR/modules/main.out" || ! printf '42\n' | cmp -s - "$TMP_DIR/modules/main.out"; then
  echo "selfhost_emit_c: FAIL module graph (post-order/dedupe/runtime)" >&2
  cat "$TMP_DIR/modules/main.err" >&2
  fail=1
fi

printf '%s\n' 'import "missing.tiq"' >"$TMP_DIR/modules/missing-main.tiq"
"$SELFHOST" "$TMP_DIR/modules/missing-main.tiq" >"$TMP_DIR/modules/missing.out" 2>"$TMP_DIR/modules/missing.err"
missing_rc=$?
if [ "$missing_rc" -eq 0 ] || [ -s "$TMP_DIR/modules/missing.out" ] ||
   ! grep -q 'missing-main.tiq:1: error\[E27\]: module not found: missing.tiq' "$TMP_DIR/modules/missing.err"; then
  echo "selfhost_emit_c: FAIL missing module (located E27/no partial output)" >&2
  cat "$TMP_DIR/modules/missing.err" >&2
  fail=1
fi

printf '%s\n' 'import "cycle-b.tiq"' >"$TMP_DIR/modules/cycle-a.tiq"
printf '%s\n' 'import "cycle-a.tiq"' >"$TMP_DIR/modules/cycle-b.tiq"
"$SELFHOST" "$TMP_DIR/modules/cycle-a.tiq" >"$TMP_DIR/modules/cycle.out" 2>"$TMP_DIR/modules/cycle.err"
cycle_rc=$?
if [ "$cycle_rc" -eq 0 ] || [ -s "$TMP_DIR/modules/cycle.out" ] ||
   ! grep -q 'cycle-b.tiq:1: error\[E28\]: circular import:' "$TMP_DIR/modules/cycle.err"; then
  echo "selfhost_emit_c: FAIL circular module (located E28/no partial output)" >&2
  cat "$TMP_DIR/modules/cycle.err" >&2
  fail=1
fi

# Unsupported backend paths must fail closed before writing partial C.
printf '%s\n' 'c = chan int' >"$TMP_DIR/unsupported_chan.tiq"
"$SELFHOST" "$TMP_DIR/unsupported_chan.tiq" >"$TMP_DIR/unsupported_chan.out" 2>"$TMP_DIR/unsupported_chan.err"
unsupported_rc=$?
if [ "$unsupported_rc" -eq 0 ] || [ -s "$TMP_DIR/unsupported_chan.out" ] ||
   ! grep -q 'unsupported_chan.tiq:1: error\[E07\]: chan is not supported yet' "$TMP_DIR/unsupported_chan.err"; then
  echo "selfhost_emit_c: FAIL unsupported_chan (did not fail closed with located E07)" >&2
  cat "$TMP_DIR/unsupported_chan.err" >&2
  fail=1
fi

# End-to-end M13.5 dogfood gate: consume the compiler's module graph, emit a
# standalone compiler, compile it strictly, and require the compiled stage to
# reproduce the exact same deterministic C bytes.
"$SELFHOST" src/tiq/emit_c_main.tiq >"$TMP_DIR/compiler.stage1.c" 2>"$TMP_DIR/compiler.stage1.err"
if [ "$?" -ne 0 ] || ! "$CC_BIN" -std=c11 -Wall -Wextra -Wpedantic -Werror \
    "$TMP_DIR/compiler.stage1.c" -o "$TMP_DIR/compiler.stage1" 2>"$TMP_DIR/compiler.stage1.cc.err"; then
  echo "selfhost_emit_c: FAIL compiler dogfood stage (emit/compile)" >&2
  cat "$TMP_DIR/compiler.stage1.err" "$TMP_DIR/compiler.stage1.cc.err" >&2
  fail=1
else
  "$TMP_DIR/compiler.stage1" src/tiq/emit_c_main.tiq >"$TMP_DIR/compiler.stage2.c" 2>"$TMP_DIR/compiler.stage2.err"
  if [ "$?" -ne 0 ] || ! cmp -s "$TMP_DIR/compiler.stage1.c" "$TMP_DIR/compiler.stage2.c"; then
    echo "selfhost_emit_c: FAIL compiler dogfood stage (self-emission identity)" >&2
    cat "$TMP_DIR/compiler.stage2.err" >&2
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "selfhost_emit_c: failed" >&2
  exit 1
fi
echo "selfhost_emit_c: ok ($count core cases + fail-closed preflight + compiler dogfood identity)"
