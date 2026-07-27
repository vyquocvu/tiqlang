#ifndef TIQ_PARSER_H
#define TIQ_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "diag.h"
#include "arena.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    DiagContext *diag;
    // Owns every AstNode and node-owned aux array (plan 4.1); released
    // wholesale by parser_free.
    Arena arena;
    bool crossed_newline;
} Parser;

void parser_init(Parser *parser, const char *source, const char *path, DiagContext *diag);
AstNode **parser_parse(Parser *parser, int *out_count);
void parser_free(Parser *parser);
void ast_print(AstNode *node, int indent);

#endif
