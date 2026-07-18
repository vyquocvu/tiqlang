#include "../include/semantic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void env_init(Environment *env, Environment *parent) {
    env->parent = parent;
    env->symbols = NULL;
    env->count = 0;
    env->capacity = 0;
}

static void env_free(Environment *env) {
    free(env->symbols);
    env->symbols = NULL;
    env->count = 0;
    env->capacity = 0;
}

static bool env_define(Environment *env, Token name, bool is_mutable, SemanticType type) {
    // Check current scope for redeclaration (we could do this but for now just insert)
    for (int i = 0; i < env->count; i++) {
        if (env->symbols[i].name.length == name.length &&
            memcmp(env->symbols[i].name.start, name.start, name.length) == 0) {
            return false; // Already defined in this scope
        }
    }

    if (env->count + 1 > env->capacity) {
        env->capacity = env->capacity < 8 ? 8 : env->capacity * 2;
        env->symbols = realloc(env->symbols, sizeof(Symbol) * env->capacity);
    }

    env->symbols[env->count].name = name;
    env->symbols[env->count].is_mutable = is_mutable;
    env->symbols[env->count].type = type;
    env->count++;
    return true;
}

static Symbol *env_lookup(Environment *env, Token name) {
    Environment *current = env;
    while (current) {
        for (int i = 0; i < current->count; i++) {
            if (current->symbols[i].name.length == name.length &&
                memcmp(current->symbols[i].name.start, name.start, name.length) == 0) {
                return &current->symbols[i];
            }
        }
        current = current->parent;
    }
    return NULL;
}

typedef struct {
    Environment *current_env;
    const char *path;
    DiagContext *diag;
} SemanticContext;

static SemanticType *alloc_type(PrimitiveType kind) {
    // In a real implementation we might arena allocate these
    SemanticType *t = malloc(sizeof(SemanticType));
    t->kind = kind;
    t->param_count = 0;
    return t;
}

static void check_node(SemanticContext *ctx, AstNode *node);

static void check_node(SemanticContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_PRINT:
            check_node(ctx, node->as.print_stmt.expr);
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        case AST_LITERAL: {
            PrimitiveType p = TYPE_UNKNOWN;
            if (node->as.literal.type == TOK_INT) p = TYPE_INT;
            else if (node->as.literal.type == TOK_FLOAT) p = TYPE_FLOAT;
            else if (node->as.literal.type == TOK_STRING) p = TYPE_STR;
            else if (node->as.literal.type == TOK_TRUE || node->as.literal.type == TOK_FALSE) p = TYPE_BOOL;
            node->semantic_type = alloc_type(p);
            break;
        }
        case AST_IDENTIFIER: {
            Symbol *sym = env_lookup(ctx->current_env, node->as.identifier.name);
            if (!sym) {
                char msg[256];
                snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNDEFINED_SYMBOL, msg);
                node->semantic_type = alloc_type(TYPE_UNKNOWN);
            } else {
                SemanticType *t = alloc_type(sym->type.kind);
                t->param_count = sym->type.param_count;
                node->semantic_type = t;
            }
            break;
        }
        case AST_BINARY: {
            check_node(ctx, node->as.binary.left);
            check_node(ctx, node->as.binary.right);

            SemanticType *lt = node->as.binary.left ? node->as.binary.left->semantic_type : NULL;
            SemanticType *rt = node->as.binary.right ? node->as.binary.right->semantic_type : NULL;

            if (lt && rt) {
                if (lt->kind != rt->kind) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "type mismatch");
                } else {
                    if (node->as.binary.op == TOK_EQ_EQ || node->as.binary.op == TOK_BANG_EQ ||
                        node->as.binary.op == TOK_LT || node->as.binary.op == TOK_LTE ||
                        node->as.binary.op == TOK_GT || node->as.binary.op == TOK_GTE) {
                        node->semantic_type = alloc_type(TYPE_BOOL);
                    } else {
                        node->semantic_type = alloc_type(lt->kind);
                    }
                }
            } else {
                node->semantic_type = alloc_type(TYPE_UNKNOWN);
            }
            break;
        }
        case AST_UNARY:
            check_node(ctx, node->as.unary.right);
            if (node->as.unary.right && node->as.unary.right->semantic_type) {
                 SemanticType *rt = node->as.unary.right->semantic_type;
                 node->semantic_type = alloc_type(rt->kind);
            } else {
                 node->semantic_type = alloc_type(TYPE_UNKNOWN);
            }
            break;
        case AST_CONDITIONAL:
            check_node(ctx, node->as.conditional.cond);
            check_node(ctx, node->as.conditional.then_branch);
            check_node(ctx, node->as.conditional.else_branch);
            node->semantic_type = alloc_type(TYPE_UNKNOWN); // Basic for now
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                if ((name.length == 2 && memcmp(name.start, "i8", 2) == 0) ||
                    (name.length == 3 && memcmp(name.start, "i16", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "i32", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "i64", 3) == 0) ||
                    (name.length == 2 && memcmp(name.start, "u8", 2) == 0) ||
                    (name.length == 3 && memcmp(name.start, "u16", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "u32", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "u64", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "f32", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "f64", 3) == 0) ||
                    (name.length == 3 && memcmp(name.start, "str", 3) == 0) ||
                    (name.length == 4 && memcmp(name.start, "bool", 4) == 0)) {

                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_CONVERSION, "unsupported conversion");
                    node->semantic_type = alloc_type(TYPE_UNKNOWN);
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        check_node(ctx, node->as.call.args[i]);
                    }
                    break;
                }
            }

            check_node(ctx, node->as.call.callee);

            if (node->as.call.callee && node->as.call.callee->semantic_type) {
                SemanticType *callee_type = (SemanticType *)node->as.call.callee->semantic_type;
                if (callee_type->param_count >= 0 && callee_type->param_count != node->as.call.arg_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "arity mismatch");
                }
            }

            for (int i = 0; i < node->as.call.arg_count; i++) {
                check_node(ctx, node->as.call.args[i]);
            }
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        case AST_BLOCK: {
            Environment block_env;
            env_init(&block_env, ctx->current_env);
            ctx->current_env = &block_env;

            for (int i = 0; i < node->as.block.stmt_count; i++) {
                check_node(ctx, node->as.block.statements[i]);
            }
            if (node->as.block.final_expr) {
                check_node(ctx, node->as.block.final_expr);
            }

            ctx->current_env = block_env.parent;
            env_free(&block_env);
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        }
        case AST_BINDING:
            check_node(ctx, node->as.binding.expr);
            {
                SemanticType type = { TYPE_UNKNOWN };
                if (node->as.binding.expr && node->as.binding.expr->semantic_type) {
                    type = *(SemanticType *)node->as.binding.expr->semantic_type;
                }
                env_define(ctx->current_env, node->as.binding.name, node->as.binding.is_mutable, type);
                node->semantic_type = alloc_type(type.kind);
            }
            break;
        case AST_ASSIGN:
            check_node(ctx, node->as.assign.expr);
            {
                Symbol *sym = env_lookup(ctx->current_env, node->as.assign.name);
                if (!sym) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                             (int)node->as.assign.name.length, node->as.assign.name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNDEFINED_SYMBOL, msg);
                } else if (!sym->is_mutable) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_IMMUTABLE_ASSIGNMENT, "cannot assign to immutable binding");
                }
            }
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        case AST_FUNCTION: {
            // Function names are available in the scope they are defined in
            SemanticType unknown_type = { TYPE_UNKNOWN };
            SemanticType func_type = { TYPE_UNKNOWN };
            func_type.param_count = node->as.function.param_count;
            env_define(ctx->current_env, node->as.function.name, false, func_type);

            Environment func_env;
            env_init(&func_env, ctx->current_env);
            ctx->current_env = &func_env;

            for (int i = 0; i < node->as.function.param_count; i++) {
                env_define(ctx->current_env, node->as.function.params[i], false, unknown_type);
            }

            check_node(ctx, node->as.function.body);

            ctx->current_env = func_env.parent;
            env_free(&func_env);
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        }
    }
}

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag) {
    SemanticContext ctx;
    ctx.path = path;
    ctx.diag = diag;

    Environment global_env;
    env_init(&global_env, NULL);
    ctx.current_env = &global_env;

    for (int i = 0; i < count; i++) {
        check_node(&ctx, stmts[i]);
    }

    env_free(&global_env);
}
