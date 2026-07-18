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

function_def  = identifier, { identifier }, "=", expression ;
binding       = identifier, ("=" | ":="), expression ;
statement     = print_stmt | assign_stmt | while_stmt | for_stmt | expression ;
print_stmt    = "!", expression ;
assign_stmt   = identifier, ("<-" | "+=" | "-=" | "*=" | "/=" | "%="), expression ;
while_stmt    = "while", expression, block ;
for_stmt      = "for", identifier, "in", expression, block ;
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
unary         = ("!" | "+" | "-"), unary | postfix ;
postfix       = primary, { call | index } ;
call          = "(", [ expression, { ",", expression } ], ")" ;
index         = "[", expression, "]" ;
primary       = identifier | literal | "(", expression, ")" | block ;
literal       = integer | float | string | "true" | "false" ;
```

Function application without parentheses, as in `fib n`, is allowed only in a function declaration parameter list. Calls use parentheses in v0.1 to avoid whitespace-sensitive ambiguity.

## Operator precedence

From tightest to loosest:

1. postfix call and index
2. unary `! + -`
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
15. assignment, which is statement-only

## Bootstrap grammar

The current bootstrap compiler implements exactly:

```ebnf
program         = spacing, { print_statement, spacing } ;
print_statement = "!", spacing_inline, string ;
spacing         = { space | tab | newline | comment } ;
spacing_inline  = { space | tab } ;
```

Unsupported input must produce a diagnostic with source position and a non-zero exit status.
