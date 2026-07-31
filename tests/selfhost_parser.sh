#!/bin/sh
# M13.3-S2: differential harness for the self-hosted Tiq parser. Builds
# src/tiq/parser_main.tiq with the C bootstrap compiler, then runs both
# `tiq dump-ast F` and the self-hosted parser over every fixture in
# examples/, examples/leetcode/, and tests/tiq/, plus an inline corpus of
# malformed sources that must fail at PARSE time. stdout, stderr, and the
# exit code must all match byte-for-byte: the C parser is the reference,
# so a partial AST on stdout plus an E04/E05/E15/E19 diagnostic on stderr
# must agree on both sides.
set -u

TIQ="${TIQ:-./build/tiq}"
SELFHOST="build/tiq-parser-selfhost"
TMP_DIR="${TMPDIR:-/tmp}/tiq-selfhost-parser-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/parser_main.tiq -o "$SELFHOST" 2> "$TMP_DIR/build.err"; then
  echo "selfhost_parser: FAIL (cannot build src/tiq/parser_main.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0
count=0

# Compare `tiq dump-ast F` against the self-hosted parser on one file.
compare() {
  src="$1"
  name="$2"
  count=$((count + 1))

  "$TIQ" dump-ast "$src" > "$TMP_DIR/$name.ref.out" 2> "$TMP_DIR/$name.ref.err"
  ref_rc=$?
  "./$SELFHOST" "$src" > "$TMP_DIR/$name.got.out" 2> "$TMP_DIR/$name.got.err"
  got_rc=$?

  if [ "$ref_rc" -ne "$got_rc" ]; then
    echo "selfhost_parser: FAIL $src (exit code: reference $ref_rc, selfhost $got_rc)" >&2
    fail=1
  fi
  if ! cmp -s "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out"; then
    echo "selfhost_parser: FAIL $src (stdout mismatch)" >&2
    diff "$TMP_DIR/$name.ref.out" "$TMP_DIR/$name.got.out" >&2 || true
    fail=1
  fi
  if ! cmp -s "$TMP_DIR/$name.ref.err" "$TMP_DIR/$name.got.err"; then
    echo "selfhost_parser: FAIL $src (stderr mismatch)" >&2
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

# Parse-error corpus: every malformed input from tests/diagnostics.sh that
# fails in the parser (not in semantic analysis), plus the structural cases
# S2 must fail closed on. Expectations are not hardcoded — the C parser's
# stdout/stderr/exit code is the reference.
err_count=0
corpus() {
  name="$1"
  source="$2"
  printf '%s' "$source" > "$TMP_DIR/$name.tiq"
  err_count=$((err_count + 1))
  compare "$TMP_DIR/$name.tiq" "err_$name"
}

# Blocks and bracket loops (E04 structural recovery).
corpus "block_no_rbrace" '{ 1'
corpus "bracket_loop_no_rbracket" '[0..10'
corpus "bracket_loop_no_lbrace" '[0..10] i'
corpus "bracket_loop_no_rbrace" '[0..10] { i'
corpus "bracket_loop_old_pipe" '[0..10 | i]'
corpus "bracket_loop_binder_missing" '[j <- 0..3, 0..2] { 0 }'
corpus "bracket_loop_dup_binder" '[j <- 0..3, j <- 0..2] { 0 }'
# Malformed enum declarations (LANGUAGE_SPEC §17.5).
corpus "enum_no_name" 'enum { A }'
corpus "enum_no_lbrace" 'enum Color A'
corpus "enum_no_rbrace" 'enum Color { A'
corpus "enum_explicit_value" 'enum Color { A = 1 }'
corpus "enum_in_block" '{ enum Color { A } }'
# Imports: string-literal operand, and the imports-first rule (§17.6).
corpus "import_not_string" 'import x'
corpus "import_after_item" 'x = 1
import "ast.tiq"'
corpus "import_bare_eof" 'import'
# M13.1-P8 container annotations (GRAMMAR container_type).
corpus "vec_annot_no_elem" 'f v:vec[ -> 1'
corpus "vec_annot_no_rbracket" 'f v:vec[int -> 1'
corpus "annot_no_type" 'f v: -> 1'
# Structs, match arms, calls, field access.
corpus "struct_no_name" 'struct { x: i64 }'
corpus "struct_no_rbrace" 'struct P { x: i64'
corpus "struct_no_field_type" 'struct P { x: }'
corpus "match_no_lbrace" 'match x 1'
corpus "match_no_fat_arrow" 'match x { 1 2 }'
corpus "match_no_rbrace" 'match x { _ => 1'
corpus "record_lit_no_rbrace" 'p = P { x: 1'
corpus "field_access_no_ident" 'p.1'
corpus "call_no_rparen" 'f(1, 2'
corpus "paren_no_rparen" 'x = (1 + 2'
corpus "index_no_rbracket" 'x = a[0'
corpus "stream_gen_no_comma" 'g = [1 2 ... a + b]'
corpus "array_fill_no_rbracket" 'a = [0; 4'
corpus "conditional_no_colon" 'x = c ? 1'
# Propagation inside a conditional branch: '?' is consumed as the conditional
# operator, so this cascades (five diagnostics) rather than parsing.
corpus "ternary_vs_propagate" 'x = c ? f(1)? : 2'
corpus "defer_top_level" 'defer print(1)'
corpus "defer_in_bracket_loop" '[0..2] { defer print(1) }'
# Termination regressions from the 0.4 fuzz findings.
corpus "fuzz_bad_index" '[0..10 | !alt[,]]'
corpus "fuzz_bad_domain" '[0(.10 | !i, break]'
corpus "fuzz_bad_args" 'f(1,,,'
corpus "fuzz_bad_match" 'match x { , => 1 }'
# Lexical failures reaching the parser as an EOF token.
corpus "unterminated_string" 'x = "hello'
corpus "bad_string_escape" 'x = "a\q"'

# Positive-construct corpus: the fixture set above never reaches SLICE,
# OMITTED, or DEFER, and covers only a few of the productions in
# src/parser.c. These are well-formed sources exercising one construct each,
# so a node layout or precedence divergence shows up as a stdout diff.
ok_count=0
construct() {
  name="$1"
  source="$2"
  printf '%s\n' "$source" > "$TMP_DIR/ok_$name.tiq"
  ok_count=$((ok_count + 1))
  compare "$TMP_DIR/ok_$name.tiq" "ok_$name"
}

# Slices and take-loops (call_or_index).
construct "slice_both" 'x = a[1..3]'
construct "slice_open_end" 'x = a[1..]'
construct "slice_open_start" 'x = a[..3]'
construct "slice_bare" 'x = a[..]'
construct "take_while" 'x = s[while c]'
construct "take_until" 'x = s[until c]'
# defer (E19 only outside a block, legal as a block statement).
construct "defer_block" 'f d:i64 -> i64 -> { defer print(1) 2 }'
construct "defer_two" 'f d:i64 -> i64 -> { defer print(1) defer print(2) 3 }'
# Postfix propagation vs. the conditional operator (both use QUESTION).
construct "propagate" 'x = f(1)? + 2'
construct "fallback" 'x = a ?? b ?? c'
# Arrays and stream generators.
construct "array_fill" 'a = [0; 4]'
construct "stream_gen" 'g = [1, 2 ... a + b]'
construct "stream_bound" 'g = [1 ... n + 1 while n < 9]'
construct "stream_until" 'g = [1 ... n + 1 until n > 9]'
construct "empty_array" 'a = []'
# Record literals and match (parsed, then reported as UNKNOWN by ast_print).
construct "record_lit" 'p = P { x: 1, y: 2 }'
construct "record_empty" 'p = P { }'
construct "match_body" 'x = match v { A => 1, _ => 2 }'
construct "match_block_body" 'x = match v { A => { 1 }, _ => 2 }'
construct "spawn" 'h = spawn f(1)'
construct "chan_typed" 'c = chan int'
construct "chan_bare" 'c = chan'
# Borrows in declaration and call position.
construct "borrow" 'f p:&P -> i64 -> 1'
construct "borrow_mut" 'f p:&mut P -> i64 -> 1'
construct "borrow_call" 'x = g(&p, &mut q)'
# M13.1-P8 container annotations (4-token peek in return position).
construct "vec_ret" 'f d:i64 -> vec[int] -> vec_new()'
construct "vec_param" 'f v:vec[Token] b:strbuf m:map -> i64 -> 1'
construct "compound_annot" 'f a:[i64; 4] -> i64 -> 1'
# Bracket loops, field access, call chains, indexed assignment.
construct "multi_binder" '[j <- 0..3, k <- 0..j] { print(int_str(j)) }'
construct "nested_field" 'x = a.b.c.d'
construct "call_chain" 'x = f(1)(2)[3].g'
construct "index_assign" 'a[0] <- 5'
construct "index_compound" 'a[i] += 2'
# Declarations.
construct "struct_def" 'struct P { x: i64, y: str }'
construct "enum_def" 'enum C { A, B, C }'
construct "move_expr" 'x = move y'
# Every precedence level in one expression, plus prefix chains.
construct "bitops" 'x = a | b ^ c & d == e < f << g + h * i'
construct "unary_chain" 'x = !-+a'
construct "shift_range" 'x = a << 1 .. b >> 2'
construct "skip_break" '[0..3] { skip break }'
construct "mut_binding" 'x <- 1'
construct "float_lit" 'x = 1.5'
construct "bool_lit" 'x = true'
construct "paren_group" 'x = (1 + 2) * 3'
construct "block_expr" 'x = { 1; 2 }'
construct "semicolons" 'f d:i64 -> i64 -> { a = 1; b = 2; a + b }'

if [ "$fixture_count" -eq 0 ]; then
  echo "selfhost_parser: FAIL (no fixtures found)" >&2
  exit 1
fi
if [ "$fail" -ne 0 ]; then
  echo "selfhost_parser: failed" >&2
  exit 1
fi
echo "selfhost_parser: ok ($fixture_count fixtures, $err_count parse-error cases, $ok_count construct cases)"
