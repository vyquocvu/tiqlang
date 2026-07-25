#ifndef TIQ_LEXER_H
#define TIQ_LEXER_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_TRUE,
    TOK_FALSE,
    TOK_WHILE,
    TOK_BREAK,
    TOK_SKIP,
    TOK_MOVE,
    TOK_DEFER,
    TOK_UNTIL,
    TOK_DOT_DOT_DOT,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_EQ_EQ,
    TOK_BANG_EQ,
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE,
    TOK_AND_AND,
    TOK_OR_OR,
    TOK_BANG,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_LSHIFT,
    TOK_RSHIFT,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,
    TOK_STAR_EQ,
    TOK_SLASH_EQ,
    TOK_PERCENT_EQ,
    TOK_COLON_EQ,
    TOK_LARROW,
    TOK_RARROW,
    TOK_QUESTION,
    TOK_COLON,
    TOK_DOT_DOT,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_NEWLINE
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;
    size_t length;
    int line;
} Token;

#include "diag.h"

typedef struct {
    const char *source;
    const char *current;
    int line;
    const char *path;
    DiagContext *diag;
} Lexer;

void lexer_init(Lexer *lexer, const char *source, const char *path, DiagContext *diag);
Token lexer_next(Lexer *lexer);
const char *token_kind_name(TokenKind kind);

#endif
