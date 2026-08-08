#!/bin/sh
# M13.4-S3: differential harness for the self-hosted Tiq semantic checker.
# Builds src/tiq/semantic_main.tiq with the C bootstrap compiler, then runs
# both `tiq dump-typed-ast F` and the self-hosted checker over every fixture
# in examples/, examples/leetcode/, and tests/tiq/, plus an inline corpus of
# sources that must fail SEMANTIC analysis (the parse-clean errors of
# tests/semantic.sh) and a positive-construct corpus reaching the types the
# fixtures never produce. stdout, stderr, and the exit code must all match
# byte-for-byte: the C checker is the reference, so the typed AST on stdout
# plus E07..E26 diagnostics on stderr must agree on both sides.
#
# Non-vacuity: after all comparisons, a histogram of TYPE_* names (stdout)
# and error[Exx] codes (stderr) over the REFERENCE outputs is printed and a
# required-coverage list is asserted, so silently-unreached checker paths
# fail the harness instead of passing vacuously.
set -u

TIQ="${TIQ:-./build/tiq}"
SELFHOST="build/tiq-semantic-selfhost"
TMP_DIR="${TMPDIR:-/tmp}/tiq-selfhost-semantic-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/semantic_main.tiq -o "$SELFHOST" 2> "$TMP_DIR/build.err"; then
  echo "selfhost_semantic: FAIL (cannot build src/tiq/semantic_main.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0
count=0

# Compare `tiq dump-typed-ast F` against the self-hosted checker on one file.
compare() {
  src="$1"
  name="$2"
  count=$((count + 1))

  "$TIQ" dump-typed-ast "$src" > "$TMP_DIR/$name.ref.out" 2> "$TMP_DIR/$name.ref.err"
  ref_rc=$?
  "./$SELFHOST" "$src" > "$TMP_DIR/$name.got.out" 2> "$TMP_DIR/$name.got.err"
  got_rc=$?

  if [ "$ref_rc" -ne "$got_rc" ]; then
    echo "selfhost_semantic: FAIL $src (exit code: reference $ref_rc, selfhost $got_rc)" >&2
    fail=1
  fi
  if ! cmp -s "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out"; then
    echo "selfhost_semantic: FAIL $src (stdout mismatch)" >&2
    diff "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" >&2 || true
    fail=1
  fi
  if ! cmp -s "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err"; then
    echo "selfhost_semantic: FAIL $src (stderr mismatch)" >&2
    diff "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err" >&2 || true
    fail=1
  fi
}

FIXTURES=$(ls examples/*.tiq tests/tiq/*.tiq 2>/dev/null)
if [ -d examples/leetcode ]; then
  FIXTURES="$FIXTURES
$(ls examples/leetcode/*.tiq 2>/dev/null)"
fi

for src in $FIXTURES; do
  compare "$src" "$(basename "$src" .tiq)"
done
fixture_count=$count

# Semantic-error corpus: every parse-clean malformed input from
# tests/semantic.sh (the parser-time failures live in selfhost_parser.sh).
# Expectations are not hardcoded - the C checker's stdout/stderr/exit code
# is the reference on both the diagnostic text AND the partial typed AST.
err_count=0
corpus() {
  name="$1"
  source="$2"
  printf '%s' "$source" > "$TMP_DIR/$name.tiq"
  err_count=$((err_count + 1))
  compare "$TMP_DIR/$name.tiq" "err_$name"
}

# E08 undefined symbols, including the multi-diagnostic cascade.
corpus "undef" 'x = y'
corpus "undef_cascade" 'x = y
z = w'
# E07 fatal statements; the second file proves fatal suppression (the E08
# after the E07 must NOT be printed).
corpus "e07_range" 'x = 1..2'
corpus "e07_suppress" 'x = 1..2
z = w'
corpus "e07_match_no_wildcard" 'v = 1
x = match v { 1 => 2 }'
corpus "e07_spawn" 'h = spawn g(1)'
corpus "e07_chan" 'c = chan int'
corpus "e07_stream_seeds" 'g = [1, 2, 3 ... x + 1]'
corpus "e07_stream_bound" 'g = [1 ... n + 1 while n < 9]'
# E09 core type mismatches.
corpus "e09_mismatch" 'x = 1 + "s"'
corpus "e09_cond_branches" 'c = true
x = c ? 1 : "s"'
corpus "e09_bang_operand" 'x = !1'
corpus "e09_array_elems" 'a = [1, "s"]'
corpus "e09_array_index" 'a = [1, 2]
x = a["s"]'
corpus "e09_index_non_array" 'x = 1
y = x[0]'
corpus "e09_slice_non_array" 'x = 1
y = x[0..1]'
corpus "e09_slice_index" 'a = [1, 2]
x = a[1..true]'
corpus "e09_string_index" 's = "abc"
x = s[true]'
corpus "e09_array_fill_len" 'a = [0; true]'
corpus "e09_print_struct" 'struct P { x: i64 }
p = P { x: 1 }
print(p)'
corpus "e09_len_int" 'x = len(1)'
corpus "e09_match_arms" 'v = 1
x = match v { 1 => 1, _ => "s" }'
corpus "e09_width_i32" 'a = i32(1)
x = a + 2'
corpus "e09_width_u8_f64" 'a = u8(1)
x = a + 1.0'
# E14/E15/E16 conditions and loops.
corpus "e14_cond" 'x = 1 ? 2 : 3'
corpus "e14_loop" 'a = 1
[a] { break }'
corpus "e15_binder" '[j <- true] { 1 }'
corpus "e16_break" 'break'
corpus "e16_skip" 'skip'
# E11/E17/E18 mutability, moves, ownership.
corpus "e11_plus_eq" 'x = 1
x += 2'
corpus "e11_index_assign" 'a = [1, 2]
a[0] <- 5'
corpus "e17_move_immutable" 'x = 1
y = move x'
corpus "e18_use_after_move" 'x <- 1
y = move x
z = x'
# E10/E12 conversions.
corpus "e10_int_to_bool" 'x = bool(1)'
corpus "e10_float_to_str" 'x = str(1.5)'
corpus "e12_conv_zero" 'x = i32()'
corpus "e12_conv_two" 'x = i32(1, 2)'
# E12 arity for print / user functions.
corpus "e12_print" 'print(1, 2)'
corpus "e12_user_fn" 'f a:i64 -> i64 -> a
x = f(1, 2)'
# E20/E21 literals and arrays.
corpus "e20_literal_range" 'x = 99999999999999999999'
corpus "e20_literal_edge" 'x = 9223372036854775808'
corpus "e21_empty_array" 'a = []'
# E23 borrow errors (LANGUAGE_SPEC 16.3).
corpus "e23_missing_borrow" 'f p:&i64 -> i64 -> 1
x <- 1
y = f(x)'
corpus "e23_by_value_borrow" 'f p:i64 -> i64 -> p
x <- 1
y = f(&x)'
corpus "e23_mut_mismatch" 'f p:&mut i64 -> i64 -> 1
x <- 1
y = f(&x)'
corpus "e23_shared_mismatch" 'f p:&i64 -> i64 -> 1
x <- 1
y = f(&mut x)'
corpus "e23_immutable_as_mut" 'f p:&mut i64 -> i64 -> 1
x = 1
y = f(&mut x)'
corpus "e23_double_mut" 'f a:&mut i64 b:&mut i64 -> i64 -> 1
x <- 1
y = f(&mut x, &mut x)'
corpus "e23_mixed_alias" 'f a:&i64 b:&mut i64 -> i64 -> 1
x <- 1
y = f(&x, &mut x)'
corpus "e23_borrow_nonident" 'f p:&i64 -> i64 -> 1
y = f(&(1 + 2))'
corpus "e07_borrow_outside_call" 'x <- 1
y = &x'
# Function annotations (P8) and return types.
corpus "e23_container_amp" 'f v:&map -> i64 -> 1'
corpus "e09_vec_annot_elem" 'f v:vec[bool] -> i64 -> 1'
corpus "e09_unknown_annot" 'f p:Foo -> i64 -> 1'
corpus "e09_unknown_ret" 'f d:i64 -> Foo -> 1'
corpus "e09_ret_mismatch" 'f d:i64 -> str -> 1'
# Enums (E24/E25/E26 and the bare-enum E09).
corpus "enum_duplicate" 'enum Color { Red }
enum Color { Blue }'
corpus "enum_struct_collision" 'struct Point { x: i64 }
enum Point { A }'
corpus "struct_enum_collision" 'enum Color { Red }
struct Color { x: i64 }'
corpus "enum_duplicate_variant" 'enum Color { Red, Green, Red }'
corpus "enum_unknown_variant" 'enum Color { Red, Green }
x = Color.Purple'
corpus "enum_bare_as_value" 'enum Color { Red }
x = Color'
# Structs and record literals.
corpus "struct_unknown_field_type" 'struct Point {
  x: unknown
}'
corpus "struct_duplicate" 'struct Point {
  x: i64
}
struct Point {
  y: i64
}'
corpus "record_lit_unknown_struct" 'p = Unknown { x: 1 }'
corpus "record_lit_unknown_field" 'struct Point {
  x: i64
}
p = Point { y: 1 }'
corpus "record_lit_field_count" 'struct Point {
  x: i64,
  y: i64
}
p = Point { x: 1 }'
corpus "field_access_non_struct" 'x = 1
y = x.foo'
corpus "field_access_unknown_field" 'struct Point {
  x: i64
}
p = Point { x: 1 }
y = p.foo'
# Option/Result.
corpus "fallback_non_option" 'x = 1
y = x ?? 0'
corpus "propagate_non_option" 'x = 1
y = x?'
corpus "some_wrong_arity" 'x = some(1, 2)'
corpus "none_literal_call" 'x = none(1)'
corpus "ok_wrong_arity" 'x = ok(1, 2)'
corpus "err_wrong_arity" 'x = err()'
# P1 str/IO builtins.
corpus "str_sub_bad_arity" 'x = str_sub("abc", 0)'
corpus "str_sub_bad_str_type" 'x = str_sub(1, 0, 1)'
corpus "str_sub_bad_index_type" 'x = str_sub("abc", "0", 1)'
corpus "str_eq_bad_arity" 'x = str_eq("a")'
corpus "str_eq_bad_type" 'x = str_eq("a", 1)'
corpus "eprint_bad_arity" 'x = eprint("a", "b")'
corpus "eprint_bad_type" 'x = eprint(42)'
corpus "fs_list_bad_arity" 'x = fs_list()'
corpus "fs_list_bad_type" 'x = fs_list(42)'
# P3 vec builtins, including the fail-closed unestablished rules and P9.
corpus "vec_new_bad_arity" 'v = vec_new(1)'
corpus "vec_push_bad_arity" 'v = vec_new()
n = vec_push(v)'
corpus "vec_set_bad_arity" 'v = vec_new()
vec_push(v, 1)
vec_set(v, 0)'
corpus "vec_len_bad_vec_type" 'x = 1
n = vec_len(x)'
corpus "vec_push_bad_vec_type" 'n = vec_push(1, 2)'
corpus "vec_push_elem_mismatch" 'v = vec_new()
vec_push(v, 1)
vec_push(v, "s")'
corpus "vec_set_elem_mismatch" 'v = vec_new()
vec_push(v, "a")
vec_set(v, 0, 1)'
corpus "vec_get_bad_index" 'v = vec_new()
vec_push(v, 1)
x = vec_get(v, "0")'
corpus "vec_push_bad_elem_kind" 'v = vec_new()
vec_push(v, 1 == 1)'
corpus "vec_get_unestablished" 'v = vec_new()
x = vec_get(v, 0)'
corpus "vec_set_unestablished" 'v = vec_new()
vec_set(v, 0, 1)'
corpus "vec_pop_unestablished" 'v = vec_new()
x = vec_pop(v)'
# P4 strbuf builtins.
corpus "strbuf_new_bad_arity" 'sb = str_buf_new(1)'
corpus "strbuf_append_bad_arity" 'sb = str_buf_new()
n = str_buf_append(sb)'
corpus "strbuf_to_str_bad_arity" 's = str_buf_to_str()'
corpus "strbuf_len_bad_arity" 'sb = str_buf_new()
n = str_buf_len(sb, 1)'
corpus "strbuf_append_bad_buf_type" 'n = str_buf_append(1, "a")'
corpus "strbuf_to_str_bad_buf_type" 'x = "s"
s = str_buf_to_str(x)'
corpus "strbuf_len_bad_buf_type" 'x = 1
n = str_buf_len(x)'
corpus "strbuf_append_bad_value_type" 'sb = str_buf_new()
str_buf_append(sb, 7)'
# P5 map builtins.
corpus "map_new_bad_arity" 'm = map_new(1)'
corpus "map_set_bad_arity" 'm = map_new()
n = map_set(m, "k")'
corpus "map_get_bad_key" 'm = map_new()
x = map_get(m, 1)'
corpus "map_set_bad_value" 'm = map_new()
n = map_set(m, "k", "v")'
corpus "map_len_bad_type" 'x = 1
n = map_len(x)'
corpus "map_key_at_bad_index" 'm = map_new()
k = map_key_at(m, "0")'
corpus "map_has_bad_map_type" 'x = 1
b = map_has(x, "k")'
# P8/P9 structural vec element checks across function boundaries.
corpus "p9_arg_elem_mismatch" 'g v:vec[int] -> i64 -> vec_len(v)
w = vec_new()
n = vec_push(w, "s")
x = g(w)'
corpus "p9_ret_elem_mismatch" 'g d:i64 -> vec[str] -> {
  v = vec_new()
  n = vec_push(v, 1)
  v
}'
# M16.1/M16.2 extern declarations: the parse-clean E29/E23/E09/E12 shapes
# of tests/semantic.sh. The ABI operand, FFI-safety, collisions, and the
# call-site arity/argument checks must agree byte-for-byte.
corpus "extern_bad_abi" 'extern "Rust" f x:i64 -> i64'
corpus "extern_unannotated_param" 'extern "C" f x -> i64'
corpus "extern_borrow_param" 'extern "C" f x:&i64 -> i64'
corpus "extern_vec_param" 'extern "C" f v:vec[int] -> i64'
corpus "extern_array_param" 'extern "C" f a:[i64; 4] -> i64'
corpus "extern_vec_return" 'extern "C" f x:i64 -> vec[int]'
corpus "extern_unknown_type" 'extern "C" f x:unknown -> i64'
corpus "extern_duplicate" 'extern "C" llabs x:i64 -> i64
extern "C" llabs y:i64 -> i64'
corpus "extern_function_collision" 'f x:i64 -> i64 -> x
extern "C" f y:i64 -> i64'
corpus "extern_struct_collision" 'struct P { x: i64 }
extern "C" P y:i64 -> i64'
corpus "extern_enum_collision" 'enum P { A }
extern "C" P y:i64 -> i64'
corpus "extern_arity" 'extern "C" llabs x:i64 -> i64
y = llabs()'
corpus "extern_arg_type" 'extern "C" llabs x:i64 -> i64
y = llabs("a")'

# Positive-construct corpus: well-formed sources exercising the types and
# checker paths the fixture set never reaches (strbuf/map/vec-of-struct in
# typed dumps, sized ints, Option/Result, borrows, the BINDING->ASSIGN
# rewrite, slices/str_view/stream). A type-pool or unification divergence
# shows up as a stdout diff on the <TYPE_*> suffixes.
ok_count=0
construct() {
  name="$1"
  source="$2"
  printf '%s\n' "$source" > "$TMP_DIR/ok_$name.tiq"
  ok_count=$((ok_count + 1))
  compare "$TMP_DIR/ok_$name.tiq" "ok_$name"
}

construct "arith" 'x = 1 + 2 * 3'
construct "float" 'x = 1.5 + 2.0'
construct "strings" 's = str_cat("a", "b")'
construct "bools" 'b = true && false'
construct "compare" 'b = 1 < 2'
construct "i64_max_edge" 'x = 9223372036854775807'
construct "conv_i8" 'a = i8(1)'
construct "conv_i16" 'a = i16(1)'
construct "conv_i32" 'a = i32(1)'
construct "conv_i64" 'a = i64(1)'
construct "conv_u8" 'a = u8(1)'
construct "conv_u16" 'a = u16(1)'
construct "conv_u32" 'a = u32(1)'
construct "conv_u64" 'a = u64(1)'
construct "conv_f32" 'a = f32(1.0)'
construct "conv_f64" 'a = f64(2.5)'
construct "conv_same" 'a = bool(true)'
construct "conv_unknown_src" 'a = i32(q)'
construct "array_index_len" 'a = [1, 2, 3]
x = a[0]
n = len(a)'
construct "array_slice" 'a = [1, 2, 3]
s = a[1..2]
t = a[1..]
u = a[..2]
w = a[..]'
construct "array_fill" 'a = [0; 4]'
construct "str_view" 's = "abc"
v = s[1..]
c = s[0]
n = len(v)'
construct "stream" 'g = [1, 2 ... (a, b) -> a + b]
x = g[0]'
construct "stream_one_seed" 'g = [1 ... (x) -> x + 1]'
construct "function_call" 'add a:i64 b:i64 -> i64 -> a + b
x = add(1, 2)'
construct "recursion" 'fib n:i64 -> i64 -> n < 2 ? n : fib(n - 1) + fib(n - 2)
x = fib(10)'
construct "block_expr" 'x = { a = 1; b = 2; a + b }'
construct "block_shadow" 'x = 1
y = { x = 2; x }
z = x'
construct "dup_binding_silent" 'x = 1
x = 2
y = x'
construct "rebind_rewrite" 'x <- 1
x <- 2'
construct "compound_assign" 'x <- 1
x += 2'
construct "index_assign" 'a <- [1, 2]
a[0] <- 5'
construct "defer_block" 'f d:i64 -> i64 -> { defer print(1) 2 }'
construct "loop_binder" '[j <- 0..3] { print(int_str(j)) }'
construct "loop_cond" 'i <- 0
[i < 3] { i += 1 }'
construct "loop_break_skip" '[i <- 0..3] { skip break }'
construct "move_ok" 'x <- 1
y = move x'
construct "borrow_shared" 'f p:&i64 -> i64 -> 1
x <- 1
y = f(&x)'
construct "borrow_mut" 'g p:&mut i64 -> i64 -> 1
x <- 1
y = g(&mut x)'
construct "struct_field" 'struct P { x: i64, y: str }
p = P { x: 1, y: "a" }
n = p.x
s = p.y'
construct "enum_variant" 'enum Color { Red, Green }
x = Color.Green'
construct "option" 'o = some(1)
x = o ?? 0
n = none
y = o?'
construct "result" 'r = ok(1)
e = err(2)
x = r ?? 0
y = r?'
construct "match_unify" 'v = 1
x = match v { 1 => 10, _ => 20 }'
construct "vec_int" 'v = vec_new()
n = vec_push(v, 7)
x = vec_get(v, 0)'
construct "vec_str" 'v = vec_new()
n = vec_push(v, "a")
s = vec_get(v, 0)
p = vec_pop(v)
m = vec_set(v, 0, "b")
c = vec_len(v)'
construct "vec_of_struct" 'struct P { x: i64 }
v = vec_new()
p = P { x: 1 }
n = vec_push(v, p)
q = vec_get(v, 0)
y = q.x'
construct "vec_annot_param" 'g v:vec[int] -> i64 -> vec_len(v)
w = vec_new()
n = vec_push(w, 1)
x = g(w)'
construct "vec_annot_ret" 'mk d:i64 -> vec[int] -> {
  v = vec_new()
  n = vec_push(v, d)
  v
}
u = mk(3)
y = vec_get(u, 0)'
construct "vec_unestablished_pass" 'g v:vec[int] -> i64 -> vec_len(v)
w = vec_new()
x = g(w)'
construct "strbuf" 'sb = str_buf_new()
n = str_buf_append(sb, "hi")
s = str_buf_to_str(sb)
m = str_buf_len(sb)'
construct "strbuf_annot" 'g b:strbuf -> i64 -> str_buf_len(b)
sb = str_buf_new()
x = g(sb)'
construct "map" 'm = map_new()
n = map_set(m, "k", 1)
x = map_get(m, "k")
h = map_has(m, "k")
k = map_key_at(m, 0)
v = map_val_at(m, 0)
c = map_len(m)'
construct "map_annot" 'g m:map -> i64 -> map_len(m)
m = map_new()
x = g(m)'
construct "str_builtins" 'x = int_str(42)
n = str_sub("abc", 0, 1)
b = str_eq("a", "a")
e = eprint("hi")'
construct "io_builtins" 'x = fs_exists("f")
y = cli_arg(0)
n = cli_arg_count()'
# M16.1/M16.2 extern positives: the decl registers with its declared return
# type and calls type-check like user functions (typed-dump goldens).
construct "extern_typed_call" 'extern "C" llabs x:i64 -> i64
y = llabs(3 - 10)'
construct "extern_zero_param" 'extern "C" getpid -> i64
p = getpid()'
construct "extern_str_f64" 'extern "C" strlen s:str -> i64
extern "C" sqrt x:f64 -> f64
n = strlen("abc")
r = sqrt(4.0)'
construct "extern_struct_by_value" 'struct Point { x: i64 }
extern "C" px p:Point -> i64
q = Point { x: 1 }'

if [ "$fixture_count" -eq 0 ]; then
  echo "selfhost_semantic: FAIL (no fixtures found)" >&2
  exit 1
fi

# Non-vacuity histograms over the REFERENCE outputs: which pooled type names
# reached a typed dump, and which diagnostic codes reached stderr.
cat "$TMP_DIR"/*.ref.out 2>/dev/null | grep -o 'TYPE_[A-Z0-9_]*' | sort | uniq -c | sort -rn > "$TMP_DIR/hist_types.txt"
cat "$TMP_DIR"/*.ref.err 2>/dev/null | grep -o 'error\[E[0-9][0-9]\]' | sort | uniq -c | sort -rn > "$TMP_DIR/hist_codes.txt"
echo "selfhost_semantic: type histogram"
cat "$TMP_DIR/hist_types.txt"
echo "selfhost_semantic: diagnostic histogram"
cat "$TMP_DIR/hist_codes.txt"

REQUIRED_TYPES="TYPE_INT TYPE_FLOAT TYPE_STR TYPE_BOOL TYPE_ARRAY TYPE_SLICE TYPE_STR_VIEW TYPE_STREAM TYPE_I8 TYPE_I16 TYPE_I32 TYPE_U8 TYPE_U16 TYPE_U32 TYPE_U64 TYPE_F32 TYPE_VEC TYPE_STRBUF TYPE_MAP TYPE_OPTION TYPE_RESULT TYPE_STRUCT TYPE_REF TYPE_REF_MUT TYPE_UNKNOWN"
for t in $REQUIRED_TYPES; do
  if ! grep -q " $t\$" "$TMP_DIR/hist_types.txt"; then
    echo "selfhost_semantic: FAIL (type $t never reached a typed dump - vacuous coverage)" >&2
    fail=1
  fi
done
REQUIRED_CODES="E07 E08 E09 E10 E11 E12 E14 E15 E16 E17 E18 E20 E21 E23 E24 E25 E26 E29"
for c in $REQUIRED_CODES; do
  if ! grep -q "error\[$c\]" "$TMP_DIR/hist_codes.txt"; then
    echo "selfhost_semantic: FAIL (diagnostic $c never emitted - vacuous coverage)" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "selfhost_semantic: failed" >&2
  exit 1
fi
echo "selfhost_semantic: ok ($fixture_count fixtures, $err_count semantic-error cases, $ok_count construct cases)"
