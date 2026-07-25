#ifndef TIQ_SEMANTIC_H
#define TIQ_SEMANTIC_H

#include "ast.h"
#include "diag.h"

typedef enum {
    TYPE_UNKNOWN,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_BOOL,
    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_STR_VIEW,
    TYPE_STREAM
} PrimitiveType;

typedef struct SemanticType {
    PrimitiveType kind;
    int param_count;
    struct SemanticType *element_type;
    int array_length;
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
