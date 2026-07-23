#ifndef TIQ_PARSER_H
#define TIQ_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "diag.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    DiagContext *diag;
    AstNode **nodes;
    int node_count;
    int node_capacity;
    bool crossed_newline;
} Parser;

void parser_init(Parser *parser, const char *source, const char *path, DiagContext *diag);
AstNode **parser_parse(Parser *parser, int *out_count);
void parser_free(Parser *parser);
void ast_print(AstNode *node, int indent);

#endif
