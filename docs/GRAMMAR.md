# Tiq Grammar

This document separates the intended language grammar from the smaller bootstrap grammar currently implemented.

## Lexical grammar

```ebnf
letter      = "A"…"Z" | "a"…"z" | "_" ;
digit       = "0"…"9" ;
identifier  = letter, { letter | digit } ;
integer     = digit, { digit | "_" } ;
float       = integer, ".", digit, { digit | "_" } ;
string      = '"', { string_char | escape }, '"' ;
escape      = "\\", ("\\" | '"' | "n" | "r" | "t" | "0") ;
comment     = "//", { any_char_except_newline } ;
```

Whitespace separates tokens. Newline is significant only as a statement separator when no grouping delimiter is open.

## Intended v0.1 grammar

Implementation status annotation (see also LANGUAGE_SPEC §17 for the complete tier table):
- ✅ **Implemented** — compiled, tested, and stable
- 🟡 **Provisional** — parsed and partially checked; semantics may change
- 🔴 **Fail-closed** — parsed but rejected at semantic analysis (no code produced)

```ebnf
program       = { top_item } ;                                      (* ✅ *)
top_item      = function_def | struct_def | binding | statement ;   (* ✅ *)

function_def  = identifier, { param }, "->", [ type, "->" ], expression ;  (* ✅ — param:type M12.4 *)
param         = identifier, [ ":", type ] ;                              (* ✅ — M12.4 *)
struct_def    = "struct", identifier, "{", { field_def, [ "," ] }, "}" ; (* ✅ — M12.6 *)
field_def     = identifier, ":", type ;                              (* ✅ — M12.6 *)
binding       = identifier, "=", expression ;                       (* ✅ *)
mutable_def   = identifier, "<-", expression ;                     (* ✅ *)
statement     = assign_stmt | bracket_loop | control_stmt | defer_stmt | expression ; (* ✅ *)
assign_stmt   = identifier, ("<-" | "+=" | "-=" | "*=" | "/=" | "%="), expression ;  (* ✅ *)
control_stmt  = "break" | "skip" ;                                  (* ✅ — loop-context checked *)
defer_stmt    = "defer", statement ;                                (* ✅ — block-context checked *)
bracket_loop  = "[", ( loop_domain | binder_clauses ), "]", "{", { statement, separator }, "}" ; (* ✅ *)
binder_clauses = identifier, "<-", loop_domain, { ",", identifier, "<-", loop_domain } ; (* ✅ *)
loop_domain   = expression ;                                        (* ✅ — range context flag set *)
block         = "{", { statement, separator }, [ expression ], "}" ; (* ✅ — E07 outside function body *)
separator     = newline | ";" ;                                     (* ✅ *)

expression    = conditional ;                                       (* ✅ *)
conditional   = fallback, [ "?", expression, ":", expression ] ;   (* ✅ — branches unified *)
fallback      = logical_or, { "??", logical_or } ;                 (* ✅ — M8: Option/Result fallback *)
logical_or    = logical_and, { "||", logical_and } ;               (* ✅ *)
logical_and   = bit_or, { "&&", bit_or } ;                        (* ✅ *)
bit_or        = bit_xor, { "|", bit_xor } ;                       (* ✅ *)
bit_xor       = bit_and, { "^", bit_and } ;                       (* ✅ *)
bit_and       = equality, { "&", equality } ;                      (* ✅ — bitwise only; unary & is 🔴 *)
equality      = comparison, { ("==" | "!="), comparison } ;        (* ✅ *)
comparison    = shift, { ("<" | "<=" | ">" | ">="), shift } ;     (* ✅ *)
shift         = range, { ("<<" | ">>"), range } ;                  (* ✅ *)
range         = additive, [ "..", additive ] ;                      (* ✅ — E07 outside loop/slice context *)
additive      = multiplicative, { ("+" | "-"), multiplicative } ;  (* ✅ *)
multiplicative = unary, { ("*" | "/" | "%"), unary } ;             (* ✅ *)
unary         = ("!" | "+" | "-" | "move"), unary | postfix ;     (* ✅ — "!" requires bool operand *)
              (* "&" unary prefix: 🔴 parsed, semantic E07 borrow unsupported (LANGUAGE_SPEC §17.3) *)
postfix       = primary, { call | index | field | propagate } ;    (* ✅ — M8: propagate = "?" *)
call          = "(", [ expression, { ",", expression } ], ")" ;   (* ✅ — builtins and user functions *)
index         = "[", ( slice_range | expression | stream_slice ), "]" ; (* ✅ / ✅ / 🔴 *)
field         = ".", identifier ;                                   (* ✅ — M12.6 struct field access *)
propagate     = "?" ;                                               (* ✅ — M8: Option/Result propagation *)
slice_range   = [ expression ], "..", [ expression ] ;             (* ✅ *)
stream_slice  = ("while" | "until"), expression ;                  (* 🔴 — bounded generators E07 *)
primary       = identifier | literal | stream_gen | array_fill | match_expr | record_lit | "(", expression, ")" | block ; (* ✅ *)
record_lit    = identifier, "{", { field_init, [ "," ] }, "}" ;    (* ✅ — M12.6 *)
field_init    = identifier, ":", expression ;                       (* ✅ — M12.6 *)
stream_gen    = "[", expression, { ",", expression }, ",", "...", expression, [ stream_bound ], "]" ; (* 🟡 — ≤2 seeds *)
stream_bound  = ("while" | "until"), expression ;                  (* 🔴 — semantic E07 bounded unsupported *)
array_fill    = "[", expression, ";", expression, "]" ;            (* ✅ — length must be integer literal *)
match_expr    = "match", expression, "{", { match_arm, [ "," ] }, "}" ; (* 🟡 — requires wildcard arm *)
match_arm     = expression, "=>", expression ;                     (* 🟡 — "_ =>" wildcard required *)
literal       = integer | float | string | "true" | "false" ;     (* ✅ — i64 range checked *)

type          = type_postfix ;                                     (* ✅ — M12.4/M12.6/M8 *)
type_postfix  = type_primary, [ "?" | "!", type_primary ] ;        (* ✅ — M8: T? = Option, T!E = Result *)
type_primary  = type_name | array_type | slice_type | "(", type, ")" ; (* ✅ *)
type_name     = "i8" | "i16" | "i32" | "i64" | "u8" | "u16" | "u32" | "u64"
              | "f32" | "f64" | "bool" | "str" | identifier ;      (* ✅ — primitive + struct types *)
array_type    = "[", type, ";", integer, "]" ;                    (* ✅ — [T; N] *)
slice_type    = "[", "]", type ;                                   (* ✅ — []T *)
```

Function application without parentheses, as in `fib n`, is allowed only in a function declaration parameter list. Calls use parentheses in v0.1 to avoid whitespace-sensitive ambiguity.

**Type annotations (M12.4)**: `function_def` parameters may have optional type annotations (`param:type`). An optional return type may follow the parameter list (`-> type -> body`). When omitted, types are inferred from use. A program whose recursive or exported function type cannot be inferred is rejected.

`!` is the logical negation prefix of `unary`; printing is the `print` builtin call (LANGUAGE_SPEC §12), covered by the ordinary `postfix` call production. Function bodies may be blocks because `block` is a `primary`, so `expression` covers both `f a -> a + 1` and `f a -> { ... }`.

## Operator precedence

From tightest to loosest:

1. postfix call and index
2. unary `! + - move`
3. `* / %`
4. `+ -`
5. `..`
6. `<< >>`
7. `< <= > >=`
8. `== !=`
9. `&`
10. `^`
11. `|`
12. `&&`
13. `||`
14. `?:`
15. `=` (immutable binding)
16. `<-` (mutable binding and reassignment)
17. `->` (function definition)

## Bootstrap grammar

The bootstrap compiler must reject all unsupported input with a diagnostic containing source position and a non-zero exit status.

The bootstrap parser additionally accepts `chan expr`, `spawn expr`, and borrow prefixes (`"&", ["mut"], unary`) for forward compatibility; all three are rejected during semantic analysis with a "not supported yet" diagnostic (fail closed, LANGUAGE_SPEC §17.3). `struct` definitions and record literals do not parse. Inline loop guards (`break if`, `skip if`) are not accepted.

See LANGUAGE_SPEC §17 for the complete four-tier surface table (Implemented / Provisional / Fail-closed / Reserved) with error codes and blocking milestones.

