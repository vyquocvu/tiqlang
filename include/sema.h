#ifndef TIQ_SEMA_H
#define TIQ_SEMA_H

#include "ast.h"
#include "diag.h"

typedef struct Symbol Symbol;
struct Symbol {
    Token name;
    Symbol *next; // for linked list in hash bucket
};

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    Symbol **buckets;
    int bucket_count;
};

typedef struct {
    DiagContext *diag;
    Scope *current_scope;
    const char *source_path;
} SemanticAnalyzer;

void sema_init(SemanticAnalyzer *analyzer, DiagContext *diag, const char *path);
void sema_analyze(SemanticAnalyzer *analyzer, AstNode **statements, int count);
void sema_free(SemanticAnalyzer *analyzer);

#endif
