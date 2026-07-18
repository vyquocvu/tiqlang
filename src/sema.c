#include "../include/sema.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void scope_init(Scope *scope, Scope *parent) {
    scope->parent = parent;
    scope->bucket_count = 16;
    scope->buckets = malloc(sizeof(Symbol *) * scope->bucket_count);
    for (int i = 0; i < scope->bucket_count; i++) {
        scope->buckets[i] = NULL;
    }
}

static void scope_free(Scope *scope) {
    for (int i = 0; i < scope->bucket_count; i++) {
        Symbol *sym = scope->buckets[i];
        while (sym) {
            Symbol *next = sym->next;
            free(sym);
            sym = next;
        }
    }
    free(scope->buckets);
}

static uint32_t hash_token(Token *token) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < token->length; i++) {
        hash ^= (uint8_t)token->start[i];
        hash *= 16777619;
    }
    return hash;
}

static bool token_equal(Token *a, Token *b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static Symbol *scope_lookup(Scope *scope, Token *name) {
    uint32_t hash = hash_token(name);
    int index = hash % scope->bucket_count;
    Symbol *sym = scope->buckets[index];
    while (sym) {
        if (token_equal(&sym->name, name)) {
            return sym;
        }
        sym = sym->next;
    }
    return NULL;
}

static void scope_define(Scope *scope, Token *name) {
    uint32_t hash = hash_token(name);
    int index = hash % scope->bucket_count;
    Symbol *sym = malloc(sizeof(Symbol));
    sym->name = *name;
    sym->next = scope->buckets[index];
    scope->buckets[index] = sym;
}

void sema_init(SemanticAnalyzer *analyzer, DiagContext *diag, const char *path) {
    analyzer->diag = diag;
    analyzer->source_path = path;
    analyzer->current_scope = malloc(sizeof(Scope));
    scope_init(analyzer->current_scope, NULL);
}

static void analyze_node(SemanticAnalyzer *analyzer, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_PRINT:
            analyze_node(analyzer, node->as.print_stmt.expr);
            break;
        case AST_LITERAL:
            break;
        case AST_IDENTIFIER: {
            bool found = false;
            for (Scope *s = analyzer->current_scope; s != NULL; s = s->parent) {
                if (scope_lookup(s, &node->as.identifier.name)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                diag_error(analyzer->diag, analyzer->source_path, node->token.line, ERR_UNDEFINED_NAME, "undefined name");
            }
            break;
        }
        case AST_BINARY:
            analyze_node(analyzer, node->as.binary.left);
            analyze_node(analyzer, node->as.binary.right);
            break;
        case AST_UNARY:
            analyze_node(analyzer, node->as.unary.right);
            break;
        case AST_CONDITIONAL:
            analyze_node(analyzer, node->as.conditional.cond);
            analyze_node(analyzer, node->as.conditional.then_branch);
            analyze_node(analyzer, node->as.conditional.else_branch);
            break;
        case AST_CALL:
            analyze_node(analyzer, node->as.call.callee);
            for (int i = 0; i < node->as.call.arg_count; i++) {
                analyze_node(analyzer, node->as.call.args[i]);
            }
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                analyze_node(analyzer, node->as.block.statements[i]);
            }
            if (node->as.block.final_expr) {
                analyze_node(analyzer, node->as.block.final_expr);
            }
            break;
        case AST_BINDING:
            if (scope_lookup(analyzer->current_scope, &node->as.binding.name)) {
                diag_error(analyzer->diag, analyzer->source_path, node->token.line, ERR_DUPLICATE_DECLAR, "duplicate declaration");
            } else {
                scope_define(analyzer->current_scope, &node->as.binding.name);
            }
            analyze_node(analyzer, node->as.binding.expr);
            break;
        case AST_ASSIGN:
            analyze_node(analyzer, node->as.assign.expr);
            break;
        case AST_FUNCTION:
            analyze_node(analyzer, node->as.function.body);
            break;
    }
}

void sema_analyze(SemanticAnalyzer *analyzer, AstNode **statements, int count) {
    for (int i = 0; i < count; i++) {
        analyze_node(analyzer, statements[i]);
    }
}

void sema_free(SemanticAnalyzer *analyzer) {
    Scope *scope = analyzer->current_scope;
    while (scope) {
        Scope *parent = scope->parent;
        scope_free(scope);
        free(scope);
        scope = parent;
    }
}
