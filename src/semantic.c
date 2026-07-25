#include "../include/semantic.h"
#include "../include/type.h"
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

static bool env_define(Environment *env, Token name, bool is_mutable, SemanticType *type) {
    for (int i = 0; i < env->count; i++) {
        if (env->symbols[i].name.length == name.length &&
            memcmp(env->symbols[i].name.start, name.start, name.length) == 0) {
            return false;
        }
    }
    if (env->count + 1 > env->capacity) {
        env->capacity = env->capacity < 8 ? 8 : env->capacity * 2;
        env->symbols = realloc(env->symbols, sizeof(Symbol) * env->capacity);
    }
    env->symbols[env->count].name = name;
    env->symbols[env->count].is_mutable = is_mutable;
    env->symbols[env->count].is_moved = false;
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
    int loop_depth;
    TypePool *pool;
} SemanticContext;

static SemanticType *ty(SemanticContext *ctx, PrimitiveType kind) {
    return type_get(ctx->pool, kind);
}

static void check_node(SemanticContext *ctx, AstNode *node);

static void check_node(SemanticContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_LITERAL: {
            PrimitiveType p = TYPE_UNKNOWN;
            if (node->as.literal.type == TOK_INT) p = TYPE_INT;
            else if (node->as.literal.type == TOK_FLOAT) p = TYPE_FLOAT;
            else if (node->as.literal.type == TOK_STRING) p = TYPE_STR;
            else if (node->as.literal.type == TOK_TRUE || node->as.literal.type == TOK_FALSE) p = TYPE_BOOL;
            node->semantic_type = ty(ctx, p);
            break;
        }
        case AST_IDENTIFIER: {
            Symbol *sym = env_lookup(ctx->current_env, node->as.identifier.name);
            if (!sym) {
                char msg[256];
                snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNDEFINED_SYMBOL, msg);
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            } else if (sym->is_moved) {
                char msg[256];
                snprintf(msg, sizeof(msg), "use of moved value '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_USE_AFTER_MOVE, msg);
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            } else {
                node->semantic_type = sym->type;
            }
            break;
        }
        case AST_BINARY: {
            check_node(ctx, node->as.binary.left);
            check_node(ctx, node->as.binary.right);
            SemanticType *lt = node->as.binary.left ? node->as.binary.left->semantic_type : NULL;
            SemanticType *rt = node->as.binary.right ? node->as.binary.right->semantic_type : NULL;
            if (lt && rt) {
                // Pooled types are immutable: propagate inference by
                // swapping node type pointers, never by mutating types.
                if (lt->kind == TYPE_UNKNOWN && rt->kind != TYPE_UNKNOWN) {
                    node->as.binary.left->semantic_type = rt;
                    lt = rt;
                } else if (rt->kind == TYPE_UNKNOWN && lt->kind != TYPE_UNKNOWN) {
                    node->as.binary.right->semantic_type = lt;
                    rt = lt;
                }
                if (lt->kind != rt->kind) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "type mismatch");
                } else {
                    if (node->as.binary.op == TOK_EQ_EQ || node->as.binary.op == TOK_BANG_EQ ||
                        node->as.binary.op == TOK_LT || node->as.binary.op == TOK_LTE ||
                        node->as.binary.op == TOK_GT || node->as.binary.op == TOK_GTE) {
                        node->semantic_type = ty(ctx, TYPE_BOOL);
                    } else {
                        node->semantic_type = ty(ctx, lt->kind);
                    }
                }
            } else {
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            break;
        }
        case AST_UNARY:
            if (node->as.unary.op == TOK_MOVE) {
                check_node(ctx, node->as.unary.right);
                if (node->as.unary.right && node->as.unary.right->kind == AST_IDENTIFIER) {
                    Token name = node->as.unary.right->as.identifier.name;
                    Symbol *sym = env_lookup(ctx->current_env, name);
                    if (sym) {
                        if (!sym->is_mutable) {
                            diag_error(ctx->diag, ctx->path, node->token.line,
                                       ERR_CANNOT_MOVE_IMMUTABLE, "cannot move an immutable binding");
                        } else if (!sym->is_moved) {
                            sym->is_moved = true;
                        }
                    }
                }
                if (node->as.unary.right && node->as.unary.right->semantic_type) {
                    node->semantic_type = node->as.unary.right->semantic_type;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            } else {
                check_node(ctx, node->as.unary.right);
                if (node->as.unary.right && node->as.unary.right->semantic_type) {
                    node->semantic_type = node->as.unary.right->semantic_type;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_CONDITIONAL:
            check_node(ctx, node->as.conditional.cond);
            check_node(ctx, node->as.conditional.then_branch);
            check_node(ctx, node->as.conditional.else_branch);
            {
                SemanticType *ct = node->as.conditional.cond ?
                    node->as.conditional.cond->semantic_type : NULL;
                if (ct && ct->kind != TYPE_BOOL) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_CONDITION_TYPE,
                               "conditional condition must be bool");
                }
                SemanticType *tt = node->as.conditional.then_branch ?
                    node->as.conditional.then_branch->semantic_type : NULL;
                SemanticType *et = node->as.conditional.else_branch ?
                    node->as.conditional.else_branch->semantic_type : NULL;
                if (tt && et) {
                    if (tt->kind == TYPE_UNKNOWN && et->kind != TYPE_UNKNOWN) {
                        node->as.conditional.then_branch->semantic_type = et;
                        tt = et;
                    } else if (et->kind == TYPE_UNKNOWN && tt->kind != TYPE_UNKNOWN) {
                        node->as.conditional.else_branch->semantic_type = tt;
                        et = tt;
                    }
                    if (tt->kind != et->kind) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "type mismatch");
                    }
                }
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                if (name.length == 3 && memcmp(name.start, "len", 3) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "len expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (node->as.call.args[0] && node->as.call.args[0]->kind == AST_IDENTIFIER) {
                            Symbol *sym = env_lookup(ctx->current_env, node->as.call.args[0]->as.identifier.name);
                            if (sym && sym->type->kind == TYPE_UNKNOWN) {
                                sym->type = type_get_slice(ctx->pool, ty(ctx, TYPE_INT));
                                node->as.call.args[0]->semantic_type = sym->type;
                                at = sym->type;
                            }
                        }
                        if (!at || (at->kind != TYPE_ARRAY && at->kind != TYPE_SLICE && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW && at->kind != TYPE_UNKNOWN))
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "len expects an array argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 7 && memcmp(name.start, "fs_read", 7) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "fs_read expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "fs_read expects a string argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_STR);
                    break;
                }
                if (name.length == 8 && memcmp(name.start, "fs_write", 8) == 0) {
                    if (node->as.call.arg_count != 2) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "fs_write expects exactly 2 arguments");
                    } else {
                        for (int i = 0; i < 2; i++) {
                            if (node->as.call.args[i]) check_node(ctx, node->as.call.args[i]);
                            SemanticType *at = node->as.call.args[i] ? node->as.call.args[i]->semantic_type : NULL;
                            if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "fs_write expects string arguments");
                        }
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 9 && memcmp(name.start, "fs_exists", 9) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "fs_exists expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "fs_exists expects a string argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_BOOL);
                    break;
                }
                if (name.length == 9 && memcmp(name.start, "proc_exec", 9) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "proc_exec expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "proc_exec expects a string argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 9 && memcmp(name.start, "proc_exit", 9) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "proc_exit expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_INT)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "proc_exit expects an int argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 14 && memcmp(name.start, "json_parse_int", 14) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "json_parse_int expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "json_parse_int expects a string argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 15 && memcmp(name.start, "json_encode_str", 15) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "json_encode_str expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "json_encode_str expects a string argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_STR);
                    break;
                }
                if (name.length == 9 && memcmp(name.start, "net_fetch", 9) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "net_fetch expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "net_fetch expects a string argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_STR);
                    break;
                }
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
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        if (node->as.call.args[i]) check_node(ctx, node->as.call.args[i]);
                    }
                    break;
                }
            }
            check_node(ctx, node->as.call.callee);
            if (node->as.call.callee && node->as.call.callee->semantic_type) {
                SemanticType *callee_type = (SemanticType *)node->as.call.callee->semantic_type;
                if (node->as.call.is_bracket_call) {
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        if (node->as.call.args[i]) check_node(ctx, node->as.call.args[i]);
                    }
                    if (node->as.call.is_slice) {
                        if (callee_type->kind == TYPE_ARRAY || callee_type->kind == TYPE_SLICE) {
                            for (int i = 0; i < 2; i++) {
                                if (node->as.call.args[i]) {
                                    SemanticType *it = node->as.call.args[i]->semantic_type;
                                    if (it && it->kind != TYPE_INT)
                                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                                   "slice index must be int");
                                }
                            }
                            node->semantic_type = type_get_slice(ctx->pool,
                                callee_type->element_type ? callee_type->element_type : ty(ctx, TYPE_INT));
                            break;
                        } else if (callee_type->kind == TYPE_STR || callee_type->kind == TYPE_STR_VIEW) {
                            for (int i = 0; i < 2; i++) {
                                if (node->as.call.args[i]) {
                                    SemanticType *it = node->as.call.args[i]->semantic_type;
                                    if (it && it->kind != TYPE_INT)
                                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                                   "slice index must be int");
                                }
                            }
                            node->semantic_type = ty(ctx, TYPE_STR_VIEW);
                            break;
                        } else if (callee_type->kind == TYPE_STREAM) {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot range-slice a stream generator");
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                            break;
                        } else {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot slice non-array");
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                            break;
                        }
                    } else if (callee_type->kind == TYPE_ARRAY || callee_type->kind == TYPE_SLICE || callee_type->kind == TYPE_UNKNOWN) {
                        if (callee_type->kind == TYPE_UNKNOWN && node->as.call.callee->kind == AST_IDENTIFIER) {
                            Symbol *sym = env_lookup(ctx->current_env, node->as.call.callee->as.identifier.name);
                            if (sym && sym->type->kind == TYPE_UNKNOWN) {
                                sym->type = type_get_slice(ctx->pool, ty(ctx, TYPE_INT));
                                node->as.call.callee->semantic_type = sym->type;
                                callee_type = sym->type;
                            }
                        }
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "array index must be int");
                        }
                        if (callee_type->element_type)
                            node->semantic_type = callee_type->element_type;
                        else
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    } else if (callee_type->kind == TYPE_STR || callee_type->kind == TYPE_STR_VIEW) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "string index must be int");
                        }
                        node->semantic_type = ty(ctx, TYPE_INT);
                        break;
                    } else if (callee_type->kind == TYPE_STREAM) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "stream index must be int");
                        }
                        node->semantic_type = ty(ctx, TYPE_INT);
                        break;
                    }
                }
                if (!node->as.call.is_bracket_call && callee_type->param_count >= 0 && callee_type->param_count != node->as.call.arg_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "arity mismatch");
                }
            }
            for (int i = 0; i < node->as.call.arg_count; i++) {
                check_node(ctx, node->as.call.args[i]);
            }
            {
                PrimitiveType ret_kind = TYPE_UNKNOWN;
                if (node->as.call.callee && node->as.call.callee->semantic_type) {
                    SemanticType *ct = node->as.call.callee->semantic_type;
                    ret_kind = ct->kind;
                }
                node->semantic_type = ty(ctx, ret_kind);
            }
            break;
        case AST_BLOCK: {
            Environment block_env;
            env_init(&block_env, ctx->current_env);
            ctx->current_env = &block_env;
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                check_node(ctx, node->as.block.statements[i]);
            }
            for (int i = 0; i < node->as.block.defer_count; i++) {
                check_node(ctx, node->as.block.deferred[i]);
            }
            if (node->as.block.final_expr) {
                check_node(ctx, node->as.block.final_expr);
            }
            ctx->current_env = block_env.parent;
            env_free(&block_env);
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        }
        case AST_BINDING:
            check_node(ctx, node->as.binding.expr);
            {
                SemanticType *type = ty(ctx, TYPE_UNKNOWN);
                if (node->as.binding.expr && node->as.binding.expr->semantic_type) {
                    type = node->as.binding.expr->semantic_type;
                }
                env_define(ctx->current_env, node->as.binding.name, node->as.binding.is_mutable, type);
                node->semantic_type = type;
            }
            break;
        case AST_ASSIGN:
            check_node(ctx, node->as.assign.expr);
            {
                Symbol *sym = env_lookup(ctx->current_env, node->as.assign.name);
                if (!sym) {
                    if ((node->as.assign.op == TOK_EQ || node->as.assign.op == TOK_LARROW) && !node->as.assign.index) {
                        SemanticType *type = ty(ctx, TYPE_UNKNOWN);
                        if (node->as.assign.expr && node->as.assign.expr->semantic_type) {
                            type = node->as.assign.expr->semantic_type;
                        }
                        bool is_mut = (node->as.assign.op == TOK_LARROW);
                        env_define(ctx->current_env, node->as.assign.name, is_mut, type);
                        node->as.assign.is_definition = true;
                        sym = env_lookup(ctx->current_env, node->as.assign.name);
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                                 (int)node->as.assign.name.length, node->as.assign.name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNDEFINED_SYMBOL, msg);
                    }
                } else if (!sym->is_mutable) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_IMMUTABLE_ASSIGNMENT, "cannot assign to immutable binding");
                } else {
                    sym->is_moved = false;
                    if (node->as.assign.index) {
                        check_node(ctx, node->as.assign.index);
                        SemanticType *it = node->as.assign.index ?
                            node->as.assign.index->semantic_type : NULL;
                        if (it && it->kind != TYPE_INT)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "array index must be int");
                        if (sym->type->kind != TYPE_ARRAY && sym->type->kind != TYPE_SLICE)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot index non-array");
                    }
                }
                if (sym) {
                    node->semantic_type = sym->type;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_FUNCTION: {
            SemanticType *func_type = type_get_func(ctx->pool, TYPE_UNKNOWN, node->as.function.param_count);
            env_define(ctx->current_env, node->as.function.name, false, func_type);
            Environment func_env;
            env_init(&func_env, ctx->current_env);
            ctx->current_env = &func_env;
            node->as.function.param_types = malloc(sizeof(void *) * (node->as.function.param_count > 0 ? node->as.function.param_count : 1));
            for (int i = 0; i < node->as.function.param_count; i++) {
                SemanticType *pt = ty(ctx, TYPE_UNKNOWN);
                node->as.function.param_types[i] = pt;
                env_define(ctx->current_env, node->as.function.params[i], false, pt);
            }
            check_node(ctx, node->as.function.body);
            for (int i = 0; i < node->as.function.param_count; i++) {
                Symbol *psym = env_lookup(ctx->current_env, node->as.function.params[i]);
                if (psym) {
                    node->as.function.param_types[i] = psym->type;
                }
            }
            ctx->current_env = func_env.parent;
            env_free(&func_env);
            PrimitiveType ret_kind = TYPE_UNKNOWN;
            if (node->as.function.body && node->as.function.body->semantic_type) {
                SemanticType *bt = (SemanticType *)node->as.function.body->semantic_type;
                ret_kind = bt->kind;
            }
            Symbol *sym = env_lookup(ctx->current_env, node->as.function.name);
            if (sym && ret_kind != TYPE_UNKNOWN) {
                sym->type = type_get_func(ctx->pool, ret_kind, node->as.function.param_count);
            }
            node->semantic_type = ty(ctx, ret_kind);
            break;
        }
        case AST_BRACKET_LOOP: {
            ctx->loop_depth++;
            check_node(ctx, node->as.bracket_loop.domain);
            bool is_range = node->as.bracket_loop.domain &&
                node->as.bracket_loop.domain->kind == AST_BINARY &&
                node->as.bracket_loop.domain->as.binary.op == TOK_DOT_DOT;
            if (is_range) {
                SemanticType *lt = node->as.bracket_loop.domain->as.binary.left ?
                    node->as.bracket_loop.domain->as.binary.left->semantic_type : NULL;
                SemanticType *rt = node->as.bracket_loop.domain->as.binary.right ?
                    node->as.bracket_loop.domain->as.binary.right->semantic_type : NULL;
                if ((lt && lt->kind != TYPE_INT) || (rt && rt->kind != TYPE_INT))
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "range bounds must be int");
            } else {
                SemanticType *dt = node->as.bracket_loop.domain ?
                    node->as.bracket_loop.domain->semantic_type : NULL;
                if (dt && dt->kind != TYPE_BOOL)
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_CONDITION_TYPE,
                               "loop condition must be bool");
            }
            Environment loop_env;
            env_init(&loop_env, ctx->current_env);
            ctx->current_env = &loop_env;
            if (is_range) {
                Token itoken;
                itoken.start = "i";
                itoken.length = 1;
                env_define(ctx->current_env, itoken, true, ty(ctx, TYPE_INT));
            }
            for (int i = 0; i < node->as.bracket_loop.body_count; i++) {
                check_node(ctx, node->as.bracket_loop.body_stmts[i]);
            }
            if (node->as.bracket_loop.body_final) {
                check_node(ctx, node->as.bracket_loop.body_final);
            }
            ctx->current_env = loop_env.parent;
            env_free(&loop_env);
            ctx->loop_depth--;
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        }
        case AST_BREAK:
            if (ctx->loop_depth == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BREAK_OUTSIDE_LOOP,
                           "break outside loop");
            }
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_SKIP:
            if (ctx->loop_depth == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BREAK_OUTSIDE_LOOP,
                           "skip outside loop");
            }
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_DEFER:
            check_node(ctx, node->as.defer.expr);
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_STREAM_GEN:
            for (int i = 0; i < node->as.stream_gen.seed_count; i++) {
                check_node(ctx, node->as.stream_gen.seeds[i]);
            }
            if (node->as.stream_gen.gen_expr) {
                Environment gen_env;
                env_init(&gen_env, ctx->current_env);
                ctx->current_env = &gen_env;
                if (node->as.stream_gen.seed_count == 1) {
                    Token xtoken;
                    xtoken.start = "x"; xtoken.length = 1;
                    env_define(ctx->current_env, xtoken, false, ty(ctx, TYPE_INT));
                }
                if (node->as.stream_gen.seed_count >= 2) {
                    Token atoken, btoken;
                    atoken.start = "a"; atoken.length = 1;
                    btoken.start = "b"; btoken.length = 1;
                    env_define(ctx->current_env, atoken, false, ty(ctx, TYPE_INT));
                    env_define(ctx->current_env, btoken, false, ty(ctx, TYPE_INT));
                }
                Token itoken, stoken;
                itoken.start = "i"; itoken.length = 1;
                stoken.start = "s"; stoken.length = 1;
                env_define(ctx->current_env, itoken, false, ty(ctx, TYPE_INT));
                env_define(ctx->current_env, stoken, false, ty(ctx, TYPE_INT));
                check_node(ctx, node->as.stream_gen.gen_expr);
                ctx->current_env = gen_env.parent;
                env_free(&gen_env);
                node->semantic_type = ty(ctx, TYPE_STREAM);
            } else {
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            if (node->as.stream_gen.bound) {
                check_node(ctx, node->as.stream_gen.bound);
            }
            break;
        case AST_BRACKET_EXPR:
            check_node(ctx, node->as.bracket_expr.expr);
            {
                SemanticType *et = node->as.bracket_expr.expr ?
                    node->as.bracket_expr.expr->semantic_type : NULL;
                if (et) {
                    node->semantic_type = ty(ctx, et->kind);
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_ARRAY: {
            PrimitiveType elem_kind = TYPE_UNKNOWN;
            bool uniform = true;
            for (int i = 0; i < node->as.array.element_count; i++) {
                check_node(ctx, node->as.array.elements[i]);
                SemanticType *et = node->as.array.elements[i] ?
                    node->as.array.elements[i]->semantic_type : NULL;
                if (et) {
                    if (i == 0) {
                        elem_kind = et->kind;
                    } else if (et->kind != elem_kind) {
                        if (et->kind != TYPE_UNKNOWN) uniform = false;
                    }
                }
            }
            if (!uniform) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                           "array elements must have the same type");
            }
            node->semantic_type = type_get_array(ctx->pool, ty(ctx, elem_kind),
                                                 node->as.array.element_count);
            break;
        }
        case AST_ARRAY_FILL: {
            check_node(ctx, node->as.array_fill.value);
            check_node(ctx, node->as.array_fill.length);
            SemanticType *vt = node->as.array_fill.value ? (SemanticType *)node->as.array_fill.value->semantic_type : NULL;
            SemanticType *lt = node->as.array_fill.length ? (SemanticType *)node->as.array_fill.length->semantic_type : NULL;
            if (lt && lt->kind != TYPE_INT) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "array fill length must be int");
            }
            int fill_len = 0;
            if (node->as.array_fill.length && node->as.array_fill.length->kind == AST_LITERAL &&
                node->as.array_fill.length->as.literal.type == TOK_INT) {
                fill_len = atoi(node->as.array_fill.length->token.start);
            }
            node->semantic_type = type_get_array(ctx->pool, ty(ctx, vt ? vt->kind : TYPE_UNKNOWN),
                                                 fill_len);
            break;
        }
        case AST_FIELD_ACCESS: {
            check_node(ctx, node->as.field_access.target);
            SemanticType *tt = node->as.field_access.target ? (SemanticType *)node->as.field_access.target->semantic_type : NULL;
            SemanticType *ft = ty(ctx, TYPE_UNKNOWN);
            if (tt && tt->kind == TYPE_STRUCT) {
                for (int i = 0; i < tt->field_count; i++) {
                    if ((int)node->as.field_access.field.length == (int)strlen(tt->field_names[i]) &&
                        memcmp(node->as.field_access.field.start, tt->field_names[i], node->as.field_access.field.length) == 0) {
                        ft = ty(ctx, tt->field_types[i] ? tt->field_types[i]->kind : TYPE_UNKNOWN);
                        break;
                    }
                }
            }
            node->semantic_type = ft;
            break;
        }
        case AST_SPAWN:
            check_node(ctx, node->as.spawn.expr);
            node->semantic_type = ty(ctx, TYPE_INT);
            break;
        case AST_CHAN:
            node->semantic_type = ty(ctx, TYPE_CHAN);
            break;
        case AST_MATCH: {
            check_node(ctx, node->as.match_expr.expr);
            PrimitiveType result_kind = TYPE_UNKNOWN;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                check_node(ctx, node->as.match_expr.arms[i].pattern);
                check_node(ctx, node->as.match_expr.arms[i].body);
                if (node->as.match_expr.arms[i].body && node->as.match_expr.arms[i].body->semantic_type) {
                    SemanticType *at = (SemanticType *)node->as.match_expr.arms[i].body->semantic_type;
                    if (result_kind == TYPE_UNKNOWN) result_kind = at->kind;
                }
            }
            node->semantic_type = ty(ctx, result_kind);
            break;
        }
        case AST_STRUCT_DEF:
        case AST_RECORD_LIT:
            node->semantic_type = ty(ctx, TYPE_STRUCT);
            break;
    }
}

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag, TypePool *pool) {
    SemanticContext ctx;
    ctx.path = path;
    ctx.diag = diag;
    ctx.loop_depth = 0;
    ctx.pool = pool;
    Environment global_env;
    env_init(&global_env, NULL);
    ctx.current_env = &global_env;
    for (int i = 0; i < count; i++) {
        check_node(&ctx, stmts[i]);
    }
    env_free(&global_env);
}
