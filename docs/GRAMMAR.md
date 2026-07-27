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

```ebnf
program       = { top_item } ;
top_item      = function_def | binding | statement ;

function_def  = identifier, { identifier }, "->", expression ;
binding       = identifier, "=", expression ;
mutable_def   = identifier, "<-", expression ;
statement     = assign_stmt | bracket_loop | control_stmt | defer_stmt | expression ;
assign_stmt   = identifier, ("<-" | "+=" | "-=" | "*=" | "/=" | "%="), expression ;
control_stmt  = "break" | "skip" ;
defer_stmt    = "defer", statement ;
bracket_loop  = "[", loop_domain, "|", { statement, separator }, [ expression ], "]" ;
loop_domain   = expression ;
block         = "{", { statement, separator }, [ expression ], "}" ;
separator     = newline | ";" ;

expression    = conditional ;
conditional   = logical_or, [ "?", expression, ":", expression ] ;
logical_or    = logical_and, { "||", logical_and } ;
logical_and   = bit_or, { "&&", bit_or } ;
bit_or        = bit_xor, { "|", bit_xor } ;
bit_xor       = bit_and, { "^", bit_and } ;
bit_and       = equality, { "&", equality } ;
equality      = comparison, { ("==" | "!="), comparison } ;
comparison    = shift, { ("<" | "<=" | ">" | ">="), shift } ;
shift         = range, { ("<<" | ">>"), range } ;
range         = additive, [ "..", additive ] ;
additive      = multiplicative, { ("+" | "-"), multiplicative } ;
multiplicative = unary, { ("*" | "/" | "%"), unary } ;
unary         = ("!" | "+" | "-" | "move"), unary | postfix ;
postfix       = primary, { call | index | field } ;
call          = "(", [ expression, { ",", expression } ], ")" ;
index         = "[", ( slice_range | expression | stream_slice ), "]" ;
field         = ".", identifier ;
slice_range   = [ expression ], "..", [ expression ] ;
stream_slice  = ("while" | "until"), expression ;
primary       = identifier | literal | stream_gen | array_fill | match_expr | "(", expression, ")" | block ;
stream_gen    = "[", expression, { ",", expression }, ",", "...", expression, [ stream_bound ], "]" ;
stream_bound = ("while" | "until"), expression ;
array_fill    = "[", expression, ";", expression, "]" ;
match_expr    = "match", expression, "{", { match_arm, [ "," ] }, "}" ;
match_arm     = expression, "=>", expression ;
literal       = integer | float | string | "true" | "false" ;
```

Function application without parentheses, as in `fib n`, is allowed only in a function declaration parameter list. Calls use parentheses in v0.1 to avoid whitespace-sensitive ambiguity.

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
