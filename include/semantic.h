#ifndef TIQ_SEMANTIC_H
#define TIQ_SEMANTIC_H

#include "ast.h"
#include "diag.h"

typedef enum {
    TYPE_UNKNOWN,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_BOOL
} PrimitiveType;

typedef struct SemanticType {
    PrimitiveType kind;
    // For function symbols, we'll store parameter count for arity checking
    int param_count;
} SemanticType;

typedef struct Symbol {
    Token name;
    bool is_mutable;
    SemanticType type;
} Symbol;

typedef struct Environment {
    struct Environment *parent;
    Symbol *symbols;
    int count;
    int capacity;
} Environment;

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag);

#endif
