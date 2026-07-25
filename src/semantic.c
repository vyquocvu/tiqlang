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
} SemanticContext;

static SemanticType *alloc_type(PrimitiveType kind) {
    SemanticType *t = malloc(sizeof(SemanticType));
    t->kind = kind;
    t->param_count = 0;
    t->element_type = NULL;
    t->array_length = 0;
    return t;
}

static void check_node(SemanticContext *ctx, AstNode *node);

static void check_node(SemanticContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_PRINT:
            check_node(ctx, node->as.print_stmt.expr);
            {
                SemanticType *pt = node->as.print_stmt.expr ?
                    node->as.print_stmt.expr->semantic_type : NULL;
                if (pt && pt->kind == TYPE_ARRAY)
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "cannot print array directly");
                if (pt && pt->kind == TYPE_STREAM)
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "cannot print stream generator directly");
            }
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
            } else if (sym->is_moved) {
                char msg[256];
                snprintf(msg, sizeof(msg), "use of moved value '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_USE_AFTER_MOVE, msg);
                node->semantic_type = alloc_type(TYPE_UNKNOWN);
            } else {
                SemanticType *t = alloc_type(sym->type.kind);
                t->param_count = sym->type.param_count;
                if (sym->type.kind == TYPE_ARRAY) {
                    t->array_length = sym->type.array_length;
                    if (sym->type.element_type)
                        t->element_type = alloc_type(sym->type.element_type->kind);
                }
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
                if (lt->kind == TYPE_UNKNOWN && rt->kind != TYPE_UNKNOWN) {
                    lt->kind = rt->kind;
                } else if (rt->kind == TYPE_UNKNOWN && lt->kind != TYPE_UNKNOWN) {
                    rt->kind = lt->kind;
                }
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
                    SemanticType *rt = node->as.unary.right->semantic_type;
                    SemanticType *t = alloc_type(rt->kind);
                    t->param_count = rt->param_count;
                    if (rt->kind == TYPE_ARRAY || rt->kind == TYPE_SLICE) {
                        t->array_length = rt->array_length;
                        if (rt->element_type)
                            t->element_type = alloc_type(rt->element_type->kind);
                    }
                    node->semantic_type = t;
                } else {
                    node->semantic_type = alloc_type(TYPE_UNKNOWN);
                }
            } else {
                check_node(ctx, node->as.unary.right);
                if (node->as.unary.right && node->as.unary.right->semantic_type) {
                    SemanticType *rt = node->as.unary.right->semantic_type;
                    SemanticType *t = alloc_type(rt->kind);
                    t->param_count = rt->param_count;
                    if (rt->kind == TYPE_ARRAY || rt->kind == TYPE_SLICE) {
                        t->array_length = rt->array_length;
                        if (rt->element_type)
                            t->element_type = alloc_type(rt->element_type->kind);
                    }
                    node->semantic_type = t;
                } else {
                    node->semantic_type = alloc_type(TYPE_UNKNOWN);
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
                        tt->kind = et->kind;
                    } else if (et->kind == TYPE_UNKNOWN && tt->kind != TYPE_UNKNOWN) {
                        et->kind = tt->kind;
                    }
                    if (tt->kind != et->kind) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "type mismatch");
                    }
                }
                node->semantic_type = alloc_type(TYPE_UNKNOWN);
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
                        if (!at || (at->kind != TYPE_ARRAY && at->kind != TYPE_SLICE && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW))
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "len expects an array argument");
                    }
                    node->semantic_type = alloc_type(TYPE_INT);
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
                    node->semantic_type = alloc_type(TYPE_UNKNOWN);
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
                            SemanticType *st = alloc_type(TYPE_SLICE);
                            if (callee_type->element_type)
                                st->element_type = alloc_type(callee_type->element_type->kind);
                            else
                                st->element_type = alloc_type(TYPE_INT);
                            node->semantic_type = st;
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
                            node->semantic_type = alloc_type(TYPE_STR_VIEW);
                            break;
                        } else if (callee_type->kind == TYPE_STREAM) {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot range-slice a stream generator");
                            node->semantic_type = alloc_type(TYPE_UNKNOWN);
                            break;
                        } else {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot slice non-array");
                            node->semantic_type = alloc_type(TYPE_UNKNOWN);
                            break;
                        }
                    } else if (callee_type->kind == TYPE_ARRAY || callee_type->kind == TYPE_SLICE) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "array index must be int");
                        }
                        if (callee_type->element_type)
                            node->semantic_type = alloc_type(callee_type->element_type->kind);
                        else
                            node->semantic_type = alloc_type(TYPE_UNKNOWN);
                        break;
                    } else if (callee_type->kind == TYPE_STR || callee_type->kind == TYPE_STR_VIEW) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "string index must be int");
                        }
                        node->semantic_type = alloc_type(TYPE_STR_VIEW);
                        break;
                    } else if (callee_type->kind == TYPE_STREAM) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "stream index must be int");
                        }
                        node->semantic_type = alloc_type(TYPE_INT);
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
                node->semantic_type = alloc_type(ret_kind);
            }
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
                SemanticType type = { TYPE_UNKNOWN, 0, NULL, 0 };
                if (node->as.binding.expr && node->as.binding.expr->semantic_type) {
                    type = *(SemanticType *)node->as.binding.expr->semantic_type;
                }
                env_define(ctx->current_env, node->as.binding.name, node->as.binding.is_mutable, type);
                SemanticType *bind_type = alloc_type(type.kind);
                if (type.kind == TYPE_ARRAY || type.kind == TYPE_SLICE) {
                    if (type.element_type)
                        bind_type->element_type = alloc_type(type.element_type->kind);
                    bind_type->array_length = type.array_length;
                }
                node->semantic_type = bind_type;
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
                } else {
                    sym->is_moved = false;
                    if (node->as.assign.index) {
                        check_node(ctx, node->as.assign.index);
                        SemanticType *it = node->as.assign.index ?
                            node->as.assign.index->semantic_type : NULL;
                        if (it && it->kind != TYPE_INT)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "array index must be int");
                        if (sym->type.kind != TYPE_ARRAY)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot index non-array");
                    }
                }
                if (node->as.assign.index && sym && sym->type.kind == TYPE_ARRAY) {
                    SemanticType *arr_type = alloc_type(TYPE_ARRAY);
                    arr_type->array_length = sym->type.array_length;
                    if (sym->type.element_type)
                        arr_type->element_type = alloc_type(sym->type.element_type->kind);
                    node->semantic_type = arr_type;
                } else {
                    node->semantic_type = alloc_type(TYPE_UNKNOWN);
                }
            }
            break;
        case AST_FUNCTION: {
            SemanticType unknown_type = { TYPE_UNKNOWN, 0, NULL, 0 };
            SemanticType func_type = { TYPE_UNKNOWN, 0, NULL, 0 };
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
            PrimitiveType ret_kind = TYPE_UNKNOWN;
            if (node->as.function.body && node->as.function.body->semantic_type) {
                SemanticType *bt = (SemanticType *)node->as.function.body->semantic_type;
                ret_kind = bt->kind;
            }
            Symbol *sym = env_lookup(ctx->current_env, node->as.function.name);
            if (sym && ret_kind != TYPE_UNKNOWN) {
                sym->type.kind = ret_kind;
            }
            node->semantic_type = alloc_type(ret_kind);
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
                SemanticType int_type = { TYPE_INT, 0, NULL, 0 };
                env_define(ctx->current_env, itoken, true, int_type);
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
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        }
        case AST_BREAK:
            if (ctx->loop_depth == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BREAK_OUTSIDE_LOOP,
                           "break outside loop");
            }
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
            break;
        case AST_SKIP:
            if (ctx->loop_depth == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BREAK_OUTSIDE_LOOP,
                           "skip outside loop");
            }
            node->semantic_type = alloc_type(TYPE_UNKNOWN);
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
                    SemanticType int_type = { TYPE_INT, 0, NULL, 0 };
                    env_define(ctx->current_env, xtoken, false, int_type);
                }
                if (node->as.stream_gen.seed_count >= 2) {
                    Token atoken, btoken;
                    atoken.start = "a"; atoken.length = 1;
                    btoken.start = "b"; btoken.length = 1;
                    SemanticType int_type = { TYPE_INT, 0, NULL, 0 };
                    env_define(ctx->current_env, atoken, false, int_type);
                    env_define(ctx->current_env, btoken, false, int_type);
                }
                Token itoken, stoken;
                itoken.start = "i"; itoken.length = 1;
                stoken.start = "s"; stoken.length = 1;
                SemanticType int_type = { TYPE_INT, 0, NULL, 0 };
                env_define(ctx->current_env, itoken, false, int_type);
                env_define(ctx->current_env, stoken, false, int_type);
                check_node(ctx, node->as.stream_gen.gen_expr);
                ctx->current_env = gen_env.parent;
                env_free(&gen_env);
                node->semantic_type = alloc_type(TYPE_STREAM);
            } else {
                node->semantic_type = alloc_type(TYPE_UNKNOWN);
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
                    node->semantic_type = alloc_type(et->kind);
                } else {
                    node->semantic_type = alloc_type(TYPE_UNKNOWN);
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
            SemanticType *arr_type = alloc_type(TYPE_ARRAY);
            arr_type->element_type = alloc_type(elem_kind);
            arr_type->array_length = node->as.array.element_count;
            node->semantic_type = arr_type;
            break;
        }
    }
}

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag) {
    SemanticContext ctx;
    ctx.path = path;
    ctx.diag = diag;
    ctx.loop_depth = 0;
    Environment global_env;
    env_init(&global_env, NULL);
    ctx.current_env = &global_env;
    for (int i = 0; i < count; i++) {
        check_node(&ctx, stmts[i]);
    }
    env_free(&global_env);
}
