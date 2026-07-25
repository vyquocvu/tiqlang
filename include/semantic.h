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
    TYPE_STREAM,
    TYPE_STRUCT,
    TYPE_OPTION,
    TYPE_RESULT,
    TYPE_REF,
    TYPE_REF_MUT,
    TYPE_CHAN,
    TYPE_UNIT
} PrimitiveType;

typedef struct SemanticType {
    PrimitiveType kind;
    int param_count;
    struct SemanticType *element_type;
    int array_length;
    char struct_name[64];
    int field_count;
    char field_names[16][32];
    struct SemanticType *field_types[16];
} SemanticType;

typedef struct Symbol {
    Token name;
    bool is_mutable;
    bool is_moved;
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
