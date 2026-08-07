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
    // M22.1: 'none' is a real reserved literal/token, like true/false, so it
    // no longer bypasses ordinary identifier lookup (issue #5 Finding 3).
    // Kept near true/false; token numbers are not persisted.
    TOK_NONE,
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
    TOK_NEWLINE,
    TOK_SPAWN,
    TOK_CHAN,
    TOK_MATCH,
    TOK_STRUCT,
    TOK_ENUM,
    TOK_MUT,
    TOK_SEMICOLON,
    TOK_FAT_ARROW,
    TOK_QUESTION_QUESTION,
    TOK_UNDERSCORE,
    TOK_DOT,
    TOK_IMPORT,
    // M16.1: 'extern' is a reserved word (LANGUAGE_SPEC §4, §7.1).
    // Appended last so existing token numbers stay stable.
    TOK_EXTERN
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;
    size_t length;
    int line;
    // Comment trivia preceding this token (NULL/0 when absent). Comments
    // never enter the main token stream; they attach to the following
    // token so the parser is undisturbed and the formatter can re-emit
    // them at their original positions.
    const char *comment_start;
    size_t comment_length;
} Token;

#include "diag.h"

typedef struct {
    const char *source;
    const char *current;
    int line;
    const char *path;
    DiagContext *diag;
    // Pending comment trivia collected while skipping whitespace,
    // attached to the next token produced.
    const char *pending_comment;
    size_t pending_comment_length;
} Lexer;

void lexer_init(Lexer *lexer, const char *source, const char *path, DiagContext *diag);
Token lexer_next(Lexer *lexer);
const char *token_kind_name(TokenKind kind);

#endif
