#!/bin/sh
set -eu

TMP_DIR="${TMPDIR:-/tmp}/tiq-lexer-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

assert_lexer() {
  name="$1"
  source="$2"
  expected="$3"
  input="$TMP_DIR/$name.tiq"
  output_file="$TMP_DIR/$name.out"
  expected_file="$TMP_DIR/$name.expected"

  printf '%s' "$source" > "$input"
  if ! ./build/tiq dump-tokens "$input" > "$output_file"; then
    echo "expected $name lexer to pass" >&2
    exit 1
  fi
  printf '%s\n' "$expected" > "$expected_file"
  if ! cmp -s "$expected_file" "$output_file"; then
    echo "lexer mismatch for $name" >&2
    echo "expected:" >&2
    cat "$expected_file" >&2
    echo "actual:" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

assert_lexer "basic" 'x = 1' 'IDENT x
EQ
INT 1'

assert_lexer "operators" '== != < <= > >= && || ! & | ^ << >> += -= *= /= %= := <- ? : ..' 'EQ_EQ
BANG_EQ
LT
LTE
GT
GTE
AND_AND
OR_OR
BANG
AMP
PIPE
CARET
LSHIFT
RSHIFT
PLUS_EQ
MINUS_EQ
STAR_EQ
SLASH_EQ
PERCENT_EQ
COLON_EQ
LARROW
QUESTION
COLON
DOT_DOT'

assert_lexer "keywords" 'true false break skip until' 'TRUE
FALSE
BREAK
SKIP
UNTIL'

assert_lexer "literals" '42 3.14 "hello"' 'INT 42
FLOAT 3.14
STRING "hello"'

assert_lexer "comments" 'x = 1 // comment
y = 2' 'IDENT x
EQ
INT 1
IDENT y
EQ
INT 2'

assert_lexer "punctuation" '{ } ( ) [ ] , ...' 'LBRACE
RBRACE
LPAREN
RPAREN
LBRACKET
RBRACKET
COMMA
DOT_DOT_DOT'

assert_lexer "math" '+ - * / %' 'PLUS
MINUS
STAR
SLASH
PERCENT'

echo "lexer: ok"
