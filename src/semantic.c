#include "../include/semantic.h"
#include "../include/type.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void die_oom(void) {
    fprintf(stderr, "out of memory\n");
    exit(1);
}

void env_init(Environment *env, Environment *parent) {
    env->parent = parent;
    env->symbols = NULL;
    env->count = 0;
    env->capacity = 0;
}

void env_free(Environment *env) {
    free(env->symbols);
    env->symbols = NULL;
    env->count = 0;
    env->capacity = 0;
}

bool env_define(Environment *env, Token name, bool is_mutable, SemanticType *type) {
    for (int i = 0; i < env->count; i++) {
        if (env->symbols[i].name.length == name.length &&
            memcmp(env->symbols[i].name.start, name.start, name.length) == 0) {
            return false;
        }
    }
    if (env->count + 1 > env->capacity) {
        int new_cap = env->capacity < 8 ? 8 : env->capacity * 2;
        Symbol *new_sym = realloc(env->symbols, sizeof(Symbol) * new_cap);
        if (!new_sym) die_oom();
        env->symbols = new_sym;
        env->capacity = new_cap;
    }
    env->symbols[env->count].name = name;
    env->symbols[env->count].is_mutable = is_mutable;
    env->symbols[env->count].is_moved = false;
    env->symbols[env->count].type = type;
    env->count++;
    return true;
}

Symbol *env_lookup(Environment *env, Token name) {
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

// M12.6: Struct registry entry
typedef struct {
    char *name;
    SemanticType *type;
} StructEntry;

// M9.1: Function registry entry; call sites need the definition to see
// per-parameter borrow kinds (the pooled function type is arity-only).
typedef struct {
    char *name;
    AstNode *node;
} FuncEntry;

typedef struct {
    Environment *current_env;
    const char *path;
    DiagContext *diag;
    int loop_depth;
    TypePool *pool;
    bool in_range_context;  // true when inside [...] loop/slice brackets
    // M12.6: struct registry
    StructEntry *structs;
    int struct_count;
    int struct_capacity;
    // M9.1: function registry
    FuncEntry *funcs;
    int func_count;
    int func_capacity;
} SemanticContext;

static SemanticType *ty(SemanticContext *ctx, PrimitiveType kind) {
    return type_get(ctx->pool, kind);
}

// Forward declaration for struct registry lookup (defined below)
static SemanticType *struct_lookup(SemanticContext *ctx, Token name);

// M12.4: Resolve a type annotation token to a SemanticType.
// Returns NULL if the token is not a valid type name.
static SemanticType *resolve_type_annot(SemanticContext *ctx, Token tok) {
    if (tok.kind != TOK_IDENT) return NULL;
    typedef struct { const char *name; int len; PrimitiveType kind; } TypeName;
    static const TypeName type_names[] = {
        {"i8",   2, TYPE_I8},    {"i16",  3, TYPE_I16},
        {"i32",  3, TYPE_I32},   {"i64",  3, TYPE_INT},
        {"u8",   2, TYPE_U8},    {"u16",  3, TYPE_U16},
        {"u32",  3, TYPE_U32},   {"u64",  3, TYPE_U64},
        {"f32",  3, TYPE_F32},   {"f64",  3, TYPE_FLOAT},
        {"bool", 4, TYPE_BOOL},  {"str",  3, TYPE_STR},
        {"int",  3, TYPE_INT},   {"float",5, TYPE_FLOAT},
    };
    for (int i = 0; i < (int)(sizeof type_names / sizeof type_names[0]); i++) {
        if ((int)tok.length == type_names[i].len &&
            memcmp(tok.start, type_names[i].name, tok.length) == 0) {
            return ty(ctx, type_names[i].kind);
        }
    }
    // M12.6: Check user-defined struct types
    SemanticType *st = struct_lookup(ctx, tok);
    if (st) return st;
    return NULL; // unknown type name
}

// M12.6: Struct registry helpers
static SemanticType *struct_lookup(SemanticContext *ctx, Token name) {
    for (int i = 0; i < ctx->struct_count; i++) {
        if ((int)strlen(ctx->structs[i].name) == (int)name.length &&
            memcmp(ctx->structs[i].name, name.start, name.length) == 0) {
            return ctx->structs[i].type;
        }
    }
    return NULL;
}

static void struct_register(SemanticContext *ctx, const char *name, SemanticType *type) {
    if (ctx->struct_count + 1 > ctx->struct_capacity) {
        int new_cap = ctx->struct_capacity < 8 ? 8 : ctx->struct_capacity * 2;
        ctx->structs = realloc(ctx->structs, sizeof(StructEntry) * (size_t)new_cap);
        if (!ctx->structs) { fprintf(stderr, "out of memory\n"); exit(1); }
        ctx->struct_capacity = new_cap;
    }
    ctx->structs[ctx->struct_count].name = strdup(name);
    ctx->structs[ctx->struct_count].type = type;
    ctx->struct_count++;
}

// M9.1: Function registry helpers.
static AstNode *func_lookup(SemanticContext *ctx, Token name) {
    // Iterate backwards so the most recent definition wins.
    for (int i = ctx->func_count - 1; i >= 0; i--) {
        if ((int)strlen(ctx->funcs[i].name) == (int)name.length &&
            memcmp(ctx->funcs[i].name, name.start, name.length) == 0) {
            return ctx->funcs[i].node;
        }
    }
    return NULL;
}

static void func_register(SemanticContext *ctx, Token name, AstNode *node) {
    if (ctx->func_count + 1 > ctx->func_capacity) {
        int new_cap = ctx->func_capacity < 8 ? 8 : ctx->func_capacity * 2;
        ctx->funcs = realloc(ctx->funcs, sizeof(FuncEntry) * (size_t)new_cap);
        if (!ctx->funcs) die_oom();
        ctx->func_capacity = new_cap;
    }
    char *s = malloc((size_t)name.length + 1);
    if (!s) die_oom();
    memcpy(s, name.start, name.length);
    s[name.length] = '\0';
    ctx->funcs[ctx->func_count].name = s;
    ctx->funcs[ctx->func_count].node = node;
    ctx->func_count++;
}

// The single kind-level compatibility rule (OPTIMIZATION_PLAN 3.1).
// Unknown unifies with anything and takes the known side; otherwise the
// kinds must match exactly. On mismatch this emits
// "<context>: expected <T>, found <U>" and returns NULL.
static SemanticType *unify(SemanticContext *ctx, int line,
                           SemanticType *expected, SemanticType *found,
                           const char *context) {
    if (!expected) return found;
    if (!found) return expected;
    if (expected->kind == TYPE_UNKNOWN) return found;
    if (found->kind == TYPE_UNKNOWN) return expected;
    if (expected->kind == found->kind) return expected;
    char want[96];
    char got[96];
    char msg[320];
    type_display(expected, want, sizeof want);
    type_display(found, got, sizeof got);
    snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
    diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
    return NULL;
}

static void check_node(SemanticContext *ctx, AstNode *node);

static void check_node(SemanticContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_LITERAL: {
            PrimitiveType p = TYPE_UNKNOWN;
            if (node->as.literal.type == TOK_INT) {
                p = TYPE_INT;
                // Integer literals default to i64 (LANGUAGE_SPEC §11);
                // out-of-range literals are rejected at compile time.
                errno = 0;
                char *end = NULL;
                (void)strtoll(node->token.start, &end, 10);
                if (errno == ERANGE || end != node->token.start + node->token.length) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_LITERAL_RANGE,
                               "integer literal out of range for i64");
                }
            }
            else if (node->as.literal.type == TOK_FLOAT) p = TYPE_FLOAT;
            else if (node->as.literal.type == TOK_STRING) p = TYPE_STR;
            else if (node->as.literal.type == TOK_TRUE || node->as.literal.type == TOK_FALSE) p = TYPE_BOOL;
            node->semantic_type = ty(ctx, p);
            break;
        }
        case AST_IDENTIFIER: {
            // M8: 'none' is a polymorphic Option constructor keyword.
            Token id_name = node->as.identifier.name;
            if (id_name.length == 4 && memcmp(id_name.start, "none", 4) == 0) {
                node->semantic_type = ty(ctx, TYPE_OPTION);
                break;
            }
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
                // M9.1: reference parameters auto-deref to the referent type
                // in expression position; the emitter re-derives ref-ness
                // from the enclosing function's parameter types.
                if (sym->type && (sym->type->kind == TYPE_REF || sym->type->kind == TYPE_REF_MUT)) {
                    node->semantic_type = sym->type->element_type ?
                        sym->type->element_type : ty(ctx, TYPE_UNKNOWN);
                } else {
                    node->semantic_type = sym->type;
                }
            }
            break;
        }
        case AST_BINARY: {
            // M12.7.2: Range expressions (a..b) are only valid inside loop/slice contexts
            if (node->as.binary.op == TOK_DOT_DOT && !ctx->in_range_context) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "range expressions 'a..b' are only valid inside loop or slice contexts");
            }
            check_node(ctx, node->as.binary.left);
            check_node(ctx, node->as.binary.right);
            SemanticType *lt = node->as.binary.left ? node->as.binary.left->semantic_type : NULL;
            SemanticType *rt = node->as.binary.right ? node->as.binary.right->semantic_type : NULL;
            // M8: Fallback operator (??) - left must be Option/Result, result is inner type
            if (node->as.binary.op == TOK_QUESTION_QUESTION) {
                if (lt && (lt->kind == TYPE_OPTION || lt->kind == TYPE_RESULT)) {
                    SemanticType *inner = lt->inner_type ? lt->inner_type : ty(ctx, TYPE_UNKNOWN);
                    // Right side should match inner type (or be unknown for inference)
                    if (rt && rt->kind != TYPE_UNKNOWN && inner->kind != TYPE_UNKNOWN) {
                        unify(ctx, node->token.line, inner, rt, "fallback type mismatch");
                    }
                    node->semantic_type = inner;
                } else {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "fallback operator requires Option or Result on left side");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
                break;
            }
            if (lt && rt) {
                // Pooled types are immutable: unify() propagates inference by
                // swapping node type pointers, never by mutating types.
                SemanticType *u = unify(ctx, node->token.line, lt, rt, "type mismatch");
                if (u) {
                    if (lt->kind == TYPE_UNKNOWN) node->as.binary.left->semantic_type = u;
                    if (rt->kind == TYPE_UNKNOWN) node->as.binary.right->semantic_type = u;
                    if (node->as.binary.op == TOK_EQ_EQ || node->as.binary.op == TOK_BANG_EQ ||
                        node->as.binary.op == TOK_LT || node->as.binary.op == TOK_LTE ||
                        node->as.binary.op == TOK_GT || node->as.binary.op == TOK_GTE) {
                        node->semantic_type = ty(ctx, TYPE_BOOL);
                    } else {
                        node->semantic_type = ty(ctx, u->kind);
                    }
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
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
            } else if (node->as.unary.op == TOK_AMP) {
                // M9.1: legal borrows (&x / &mut x as arguments to reference
                // parameters) are consumed inside AST_CALL. Reaching this case
                // means the borrow appears anywhere else: fail closed, borrows
                // cannot be stored, returned, or re-borrowed (LANGUAGE_SPEC §16.3).
                check_node(ctx, node->as.unary.right);
                diag_error(ctx->diag, ctx->path, node->token.line,
                           ERR_UNSUPPORTED_STATEMENT,
                           "borrow is only valid as an argument to a reference parameter");
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            } else if (node->as.unary.op == TOK_QUESTION) {
                // M8: Propagation operator (expr?) - unwraps Option/Result.
                check_node(ctx, node->as.unary.right);
                SemanticType *rt = node->as.unary.right ?
                    node->as.unary.right->semantic_type : NULL;
                if (rt && (rt->kind == TYPE_OPTION || rt->kind == TYPE_RESULT)) {
                    node->semantic_type = rt->inner_type ? rt->inner_type : ty(ctx, TYPE_UNKNOWN);
                } else {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "propagation operator requires Option or Result operand");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            } else {
                check_node(ctx, node->as.unary.right);
                if (node->as.unary.op == TOK_BANG) {
                    // '!' is logical negation only (print is the print
                    // builtin, LANGUAGE_SPEC §12). Tiq has no truthiness,
                    // so the operand must already be bool.
                    SemanticType *rt = node->as.unary.right ?
                        node->as.unary.right->semantic_type : NULL;
                    if (rt && rt->kind != TYPE_BOOL && rt->kind != TYPE_UNKNOWN) {
                        char disp[96];
                        char msg[160];
                        type_display(rt, disp, sizeof disp);
                        snprintf(msg, sizeof msg, "operand of '!' must be bool, found %s", disp);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                    }
                    node->semantic_type = ty(ctx, TYPE_BOOL);
                } else if (node->as.unary.right && node->as.unary.right->semantic_type) {
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
                    SemanticType *u = unify(ctx, node->token.line, tt, et, "type mismatch");
                    if (u) {
                        if (tt->kind == TYPE_UNKNOWN) node->as.conditional.then_branch->semantic_type = u;
                        if (et->kind == TYPE_UNKNOWN) node->as.conditional.else_branch->semantic_type = u;
                    }
                }
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                if (name.length == 5 && memcmp(name.start, "print", 5) == 0) {
                    // print builtin (LANGUAGE_SPEC §12): one argument of a
                    // printable type; returns the number of bytes written.
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "print expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_INT && at->kind != TYPE_FLOAT &&
                            at->kind != TYPE_BOOL && at->kind != TYPE_STR &&
                            at->kind != TYPE_STR_VIEW && at->kind != TYPE_SLICE &&
                            at->kind != TYPE_I8 && at->kind != TYPE_I16 &&
                            at->kind != TYPE_I32 && at->kind != TYPE_U8 &&
                            at->kind != TYPE_U16 && at->kind != TYPE_U32 &&
                            at->kind != TYPE_U64 && at->kind != TYPE_F32 &&
                            at->kind != TYPE_UNKNOWN) {
                            char disp[96];
                            char msg[160];
                            type_display(at, disp, sizeof disp);
                            snprintf(msg, sizeof msg, "print cannot print %s", disp);
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                        }
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 3 && memcmp(name.start, "len", 3) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "len expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (!at || (at->kind != TYPE_ARRAY && at->kind != TYPE_SLICE && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW && at->kind != TYPE_UNKNOWN))
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "len expects an array argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                // M8: Option constructors
                if (name.length == 4 && memcmp(name.start, "some", 4) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "some expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_UNKNOWN) {
                            node->semantic_type = type_get_option(ctx->pool, at);
                        } else {
                            node->semantic_type = ty(ctx, TYPE_OPTION);
                        }
                    }
                    break;
                }
                if (name.length == 4 && memcmp(name.start, "none", 4) == 0) {
                    if (node->as.call.arg_count != 0) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "none expects no arguments");
                    }
                    // none is polymorphic - type inferred from context
                    node->semantic_type = ty(ctx, TYPE_OPTION);
                    break;
                }
                // M8: Result constructors
                if (name.length == 2 && memcmp(name.start, "ok", 2) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "ok expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_UNKNOWN) {
                            node->semantic_type = type_get_result(ctx->pool, at, ty(ctx, TYPE_INT));
                        } else {
                            node->semantic_type = ty(ctx, TYPE_RESULT);
                        }
                    }
                    break;
                }
                if (name.length == 3 && memcmp(name.start, "err", 3) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "err expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_UNKNOWN) {
                            node->semantic_type = type_get_result(ctx->pool, ty(ctx, TYPE_INT), at);
                        } else {
                            node->semantic_type = ty(ctx, TYPE_RESULT);
                        }
                    }
                    break;
                }
                {
                    typedef struct {
                        const char *name; int name_len; int arity;
                        PrimitiveType expected; PrimitiveType ret;
                        // M10.3: optional distinct type for the second argument;
                        // TYPE_UNKNOWN (0) means "same as expected".
                        PrimitiveType expected2;
                    } Builtin;
                    static const Builtin builtins[] = {
                        {"fs_read",        7, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN},
                        {"fs_write",       8, 2, TYPE_STR, TYPE_INT, TYPE_UNKNOWN},
                        {"fs_exists",      9, 1, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN},
                        {"proc_exec",      9, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN},
                        {"proc_exit",      9, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN},
                        {"json_parse_int",14, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN},
                        {"json_encode_str",15,1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN},
                        // M10.2: JSON decoder builtin (LANGUAGE_SPEC §19).
                        {"json_get",       8, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN},
                        // M10.3: JSON array builtins (LANGUAGE_SPEC §19).
                        {"json_arr_len",  12, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN},
                        {"json_arr_get",  12, 2, TYPE_STR, TYPE_STR, TYPE_INT},
                        {"net_fetch",      9, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN},
                        // M10.1: CLI argument builtins (LANGUAGE_SPEC §18.1)
                        {"cli_arg_count", 13, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN},
                        {"cli_arg",        7, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN},
                    };
                    bool matched = false;
                    for (int bi = 0; bi < (int)(sizeof builtins / sizeof builtins[0]); bi++) {
                        if ((int)name.length != builtins[bi].name_len ||
                            memcmp(name.start, builtins[bi].name, name.length) != 0)
                            continue;
                        matched = true;
                        if (node->as.call.arg_count != builtins[bi].arity) {
                            char msg[128];
                            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                                     builtins[bi].name, builtins[bi].arity,
                                     builtins[bi].arity == 1 ? "" : "s");
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
                        } else {
                            for (int ai = 0; ai < node->as.call.arg_count; ai++) {
                                if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
                            }
                            for (int ai = 0; ai < node->as.call.arg_count; ai++) {
                                if (!node->as.call.args[ai]) continue;
                                SemanticType *at = node->as.call.args[ai]->semantic_type;
                                if (!at) continue;
                                PrimitiveType want = builtins[bi].expected;
                                if (ai == 1 && builtins[bi].expected2 != TYPE_UNKNOWN)
                                    want = builtins[bi].expected2;
                                // str parameters also accept borrowed str_view slices.
                                if (want == TYPE_STR && at->kind == TYPE_STR_VIEW)
                                    continue;
                                char where[64];
                                snprintf(where, sizeof where, "%s argument", builtins[bi].name);
                                unify(ctx, node->token.line, ty(ctx, want), at, where);
                            }
                        }
                        node->semantic_type = ty(ctx, builtins[bi].ret);
                        break;
                    }
                    if (matched) break;
                }
                // M12.3: Explicit numeric type conversions.
                // type_name(expr) where type_name is a primitive type keyword
                // is a real checked conversion, not a function call.
                // Allowlist: numeric <-> numeric always permitted (C cast,
                // narrowing is the programmer's explicit intent).
                // bool <-> numeric and str <-> anything are rejected (E10).
                typedef struct {
                    const char *name; int len; PrimitiveType kind; const char *display;
                } ConvEntry;
                static const ConvEntry conv_table[] = {
                    {"i8",   2, TYPE_I8,    "i8"},
                    {"i16",  3, TYPE_I16,   "i16"},
                    {"i32",  3, TYPE_I32,   "i32"},
                    {"i64",  3, TYPE_INT,   "i64"},
                    {"u8",   2, TYPE_U8,    "u8"},
                    {"u16",  3, TYPE_U16,   "u16"},
                    {"u32",  3, TYPE_U32,   "u32"},
                    {"u64",  3, TYPE_U64,   "u64"},
                    {"f32",  3, TYPE_F32,   "f32"},
                    {"f64",  3, TYPE_FLOAT, "f64"},
                    {"bool", 4, TYPE_BOOL,  "bool"},
                    {"str",  3, TYPE_STR,   "str"},
                };
                static const int conv_count = (int)(sizeof conv_table / sizeof conv_table[0]);
                int ci = -1;
                for (int k = 0; k < conv_count; k++) {
                    if ((int)name.length == conv_table[k].len &&
                        memcmp(name.start, conv_table[k].name, (size_t)conv_table[k].len) == 0) {
                        ci = k; break;
                    }
                }
                if (ci >= 0) {
                    PrimitiveType tgt = conv_table[ci].kind;
                    // Arity check: exactly one argument.
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_ARITY_MISMATCH,
                                   "type conversion requires exactly 1 argument");
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (node->as.call.args[i]) check_node(ctx, node->as.call.args[i]);
                        }
                        break;
                    }
                    // Type-check the argument.
                    if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                    SemanticType *src_t = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                    PrimitiveType src = src_t ? src_t->kind : TYPE_UNKNOWN;
                    // Determine whether the conversion is in the allowlist.
                    // Numeric kinds: INT, FLOAT, I8, I16, I32, U8, U16, U32, U64, F32.
                    // (TYPE_I64 == TYPE_INT, TYPE_F64 == TYPE_FLOAT)
                    bool src_numeric = (src == TYPE_INT  || src == TYPE_FLOAT ||
                                        src == TYPE_I8   || src == TYPE_I16   ||
                                        src == TYPE_I32  || src == TYPE_U8    ||
                                        src == TYPE_U16  || src == TYPE_U32   ||
                                        src == TYPE_U64  || src == TYPE_F32);
                    bool tgt_numeric = (tgt == TYPE_INT  || tgt == TYPE_FLOAT ||
                                        tgt == TYPE_I8   || tgt == TYPE_I16   ||
                                        tgt == TYPE_I32  || tgt == TYPE_U8    ||
                                        tgt == TYPE_U16  || tgt == TYPE_U32   ||
                                        tgt == TYPE_U64  || tgt == TYPE_F32);
                    bool allowed = (src == TYPE_UNKNOWN)       /* unknown propagation */
                                || (src_numeric && tgt_numeric)/* numeric <-> numeric */
                                || (src == tgt);               /* identity (bool->bool, str->str) */
                    if (!allowed) {
                        // Produce a clear E10 message naming source and target types.
                        char src_name[32]; char msg[128];
                        type_display(src_t, src_name, sizeof src_name);
                        snprintf(msg, sizeof msg, "cannot convert %s to %s",
                                 src_name, conv_table[ci].display);
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_UNSUPPORTED_CONVERSION, msg);
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    }
                    node->semantic_type = ty(ctx, tgt);
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
                // M12.6: Skip arity check for struct-returning functions
                // (param_count is 0 for struct types; arity checked at definition)
                if (!node->as.call.is_bracket_call && callee_type->kind != TYPE_STRUCT &&
                    callee_type->param_count >= 0 && callee_type->param_count != node->as.call.arg_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "arity mismatch");
                }
            }
            {
                // M9.1: borrow-aware argument checking. The callee definition
                // (if known) supplies per-parameter borrow kinds; borrows are
                // only legal where the parameter is a reference (§16.3).
                AstNode *fn_def = NULL;
                if (!node->as.call.is_bracket_call && node->as.call.callee &&
                    node->as.call.callee->kind == AST_IDENTIFIER) {
                    fn_def = func_lookup(ctx, node->as.call.callee->as.identifier.name);
                }
                // Per-call aliasing bookkeeping: referent name + borrow kind.
                typedef struct { Token name; bool is_mut; } BorrowRec;
                BorrowRec *recs = NULL;
                int rec_count = 0;
                if (node->as.call.arg_count > 0) {
                    recs = malloc(sizeof(BorrowRec) * (size_t)node->as.call.arg_count);
                    if (!recs) die_oom();
                }
                for (int i = 0; i < node->as.call.arg_count; i++) {
                    AstNode *arg = node->as.call.args[i];
                    if (!arg) continue;
                    bool arg_is_borrow = arg->kind == AST_UNARY &&
                                         arg->as.unary.op == TOK_AMP;
                    int want_ref = 0; // 0=value, 1=&, 2=&mut
                    if (fn_def && i < fn_def->as.function.param_count &&
                        fn_def->as.function.param_ref_kinds)
                        want_ref = fn_def->as.function.param_ref_kinds[i];
                    if (!arg_is_borrow) {
                        check_node(ctx, arg);
                        if (want_ref != 0) {
                            char msg[128];
                            snprintf(msg, sizeof msg, "argument %d must be borrowed with %s",
                                     i + 1, want_ref == 2 ? "&mut" : "&");
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                        continue;
                    }
                    if (want_ref == 0) {
                        char msg[128];
                        snprintf(msg, sizeof msg,
                                 "argument %d cannot be a borrow: parameter is by value", i + 1);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        if (arg->as.unary.right) check_node(ctx, arg->as.unary.right);
                        arg->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        continue;
                    }
                    bool is_mut = arg->as.unary.is_mut_borrow;
                    if ((want_ref == 2) != is_mut) {
                        char msg[128];
                        snprintf(msg, sizeof msg, "argument %d must be borrowed with %s",
                                 i + 1, want_ref == 2 ? "&mut" : "&");
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                    }
                    AstNode *operand = arg->as.unary.right;
                    if (!operand || operand->kind != AST_IDENTIFIER) {
                        diag_error(ctx->diag, ctx->path, arg->token.line, ERR_BORROW,
                                   "borrow operand must be a named binding");
                        if (operand) check_node(ctx, operand);
                        arg->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        continue;
                    }
                    // Reports undefined symbol / use-after-move and yields the
                    // referent type (auto-deref never fires here except for
                    // re-borrows, which are rejected below).
                    check_node(ctx, operand);
                    Token ref_name = operand->as.identifier.name;
                    Symbol *sym = env_lookup(ctx->current_env, ref_name);
                    if (sym) {
                        if (sym->type && (sym->type->kind == TYPE_REF ||
                                          sym->type->kind == TYPE_REF_MUT)) {
                            char msg[256];
                            snprintf(msg, sizeof msg, "cannot re-borrow reference parameter '%.*s'",
                                     (int)ref_name.length, ref_name.start);
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                        if (is_mut && !sym->is_mutable) {
                            char msg[256];
                            snprintf(msg, sizeof msg, "cannot borrow immutable binding '%.*s' as mutable",
                                     (int)ref_name.length, ref_name.start);
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                    }
                    // Referent type must match the declared element type.
                    if (fn_def && i < fn_def->as.function.param_count &&
                        fn_def->as.function.param_types &&
                        fn_def->as.function.param_types[i]) {
                        SemanticType *pt = fn_def->as.function.param_types[i];
                        if ((pt->kind == TYPE_REF || pt->kind == TYPE_REF_MUT) &&
                            pt->element_type) {
                            char where[64];
                            snprintf(where, sizeof where, "argument %d", i + 1);
                            unify(ctx, node->token.line, pt->element_type,
                                  operand->semantic_type, where);
                        }
                    }
                    // Aliasing within a single call: any number of shared
                    // borrows, at most one mutable, never mixed (§16.3).
                    for (int r = 0; r < rec_count; r++) {
                        if (recs[r].name.length == ref_name.length &&
                            memcmp(recs[r].name.start, ref_name.start, ref_name.length) == 0) {
                            if (recs[r].is_mut && is_mut) {
                                char msg[256];
                                snprintf(msg, sizeof msg, "cannot borrow '%.*s' as mutable more than once in a call",
                                         (int)ref_name.length, ref_name.start);
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                                break;
                            } else if (recs[r].is_mut || is_mut) {
                                char msg[256];
                                snprintf(msg, sizeof msg, "cannot borrow '%.*s' as both mutable and shared in a call",
                                         (int)ref_name.length, ref_name.start);
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                                break;
                            }
                        }
                    }
                    recs[rec_count].name = ref_name;
                    recs[rec_count].is_mut = is_mut;
                    rec_count++;
                    arg->semantic_type = type_get_ref(ctx->pool,
                        operand->semantic_type ? operand->semantic_type : ty(ctx, TYPE_UNKNOWN),
                        is_mut);
                }
                free(recs);
            }
            {
                // M12.6: Use full callee type for struct returns (preserves struct_name)
                if (node->as.call.callee && node->as.call.callee->semantic_type) {
                    SemanticType *ct = node->as.call.callee->semantic_type;
                    node->semantic_type = ct;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
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
                } else if (sym->type && sym->type->kind == TYPE_REF) {
                    // M9.1: shared borrows are read-only views.
                    char msg[256];
                    snprintf(msg, sizeof msg, "cannot assign through shared borrow '%.*s'",
                             (int)node->as.assign.name.length, node->as.assign.name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                } else if (sym->type && sym->type->kind == TYPE_REF_MUT) {
                    // M9.1: assignment through a mutable borrow mutates the
                    // referent in the caller; the emitter dereferences.
                    sym->is_moved = false;
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
            // M9.1: record the definition so call sites can see borrow kinds.
            func_register(ctx, node->as.function.name, node);
            Environment func_env;
            env_init(&func_env, ctx->current_env);
            ctx->current_env = &func_env;
            // M12.4: Use declared parameter types from annotations if present,
            // otherwise use TYPE_UNKNOWN for inference.
            for (int i = 0; i < node->as.function.param_count; i++) {
                SemanticType *pt = ty(ctx, TYPE_UNKNOWN);
                // Check for type annotation
                if (node->as.function.param_type_annots &&
                    node->as.function.param_type_annots[i].kind == TOK_IDENT) {
                    SemanticType *annot = resolve_type_annot(ctx, node->as.function.param_type_annots[i]);
                    if (annot) {
                        pt = annot;
                    } else {
                        char msg[128];
                        snprintf(msg, sizeof msg, "unknown type '%.*s'",
                                 (int)node->as.function.param_type_annots[i].length,
                                 node->as.function.param_type_annots[i].start);
                        diag_error(ctx->diag, ctx->path, node->as.function.param_type_annots[i].line,
                                   ERR_TYPE_MISMATCH, msg);
                    }
                }
                // M9.1: wrap reference parameters as &T / &mut T.
                if (node->as.function.param_ref_kinds &&
                    node->as.function.param_ref_kinds[i] != 0) {
                    pt = type_get_ref(ctx->pool, pt,
                                      node->as.function.param_ref_kinds[i] == 2);
                }
                if (node->as.function.param_types) node->as.function.param_types[i] = pt;
                env_define(ctx->current_env, node->as.function.params[i], false, pt);
            }
            check_node(ctx, node->as.function.body);
            for (int i = 0; i < node->as.function.param_count; i++) {
                Symbol *psym = env_lookup(ctx->current_env, node->as.function.params[i]);
                if (psym && node->as.function.param_types) {
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
            // M12.4: Check body against declared return type if present
            SemanticType *ret_type = NULL; // Full type for struct returns
            if (node->as.function.return_type_annot.kind == TOK_IDENT) {
                SemanticType *ret_annot = resolve_type_annot(ctx, node->as.function.return_type_annot);
                if (ret_annot) {
                    SemanticType *u = unify(ctx, node->token.line, ret_annot,
                                            ty(ctx, ret_kind), "return type mismatch");
                    if (u) ret_kind = u->kind;
                    // M12.6: Preserve full struct type for return
                    if (ret_annot->kind == TYPE_STRUCT) ret_type = ret_annot;
                } else {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown return type '%.*s'",
                             (int)node->as.function.return_type_annot.length,
                             node->as.function.return_type_annot.start);
                    diag_error(ctx->diag, ctx->path, node->as.function.return_type_annot.line,
                               ERR_TYPE_MISMATCH, msg);
                }
            }
            Symbol *sym = env_lookup(ctx->current_env, node->as.function.name);
            if (sym) {
                // Unify the previously recorded return type (unknown for a
                // fresh definition) with the body type before re-pointing.
                SemanticType *u = unify(ctx, node->token.line,
                                        ty(ctx, sym->type ? sym->type->kind : TYPE_UNKNOWN),
                                        ty(ctx, ret_kind), "type mismatch");
                if (u && u->kind != TYPE_UNKNOWN) {
                    // M12.6: For struct returns, use the full struct type so
                    // callers can access fields. param_count is lost but
                    // arity is checked at the definition site.
                    if (ret_type && ret_type->kind == TYPE_STRUCT) {
                        sym->type = ret_type;
                    } else {
                        sym->type = type_get_func(ctx->pool, u->kind, node->as.function.param_count);
                    }
                }
            }
            // M12.6: Use full struct type if available, otherwise use kind-only type
            node->semantic_type = ret_type ? ret_type : ty(ctx, ret_kind);
            break;
        }
        case AST_BRACKET_LOOP: {
            ctx->loop_depth++;
            // M12.7.2: Set range context for domain checking
            bool was_in_range = ctx->in_range_context;
            ctx->in_range_context = true;
            check_node(ctx, node->as.bracket_loop.domain);
            ctx->in_range_context = was_in_range;
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
            if (node->as.bracket_loop.has_binder && !is_range) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                           "loop binder requires a range domain");
            }
            Environment loop_env;
            env_init(&loop_env, ctx->current_env);
            ctx->current_env = &loop_env;
            if (node->as.bracket_loop.has_binder) {
                // The binder replaces the implicit index; loop variables
                // are immutable inside the body.
                env_define(ctx->current_env, node->as.bracket_loop.binder, false,
                           ty(ctx, TYPE_INT));
            } else if (is_range) {
                Token itoken;
                itoken.start = "i";
                itoken.length = 1;
                env_define(ctx->current_env, itoken, false, ty(ctx, TYPE_INT));
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
            // Stream generators support 1 or 2 seeds only (v0.1 window size)
            if (node->as.stream_gen.seed_count > 2) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "stream generators support at most 2 seeds");
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                break;
            }
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
                if (node->as.stream_gen.seed_count == 2) {
                    Token atoken, btoken;
                    atoken.start = "a"; atoken.length = 1;
                    btoken.start = "b"; btoken.length = 1;
                    env_define(ctx->current_env, atoken, false, ty(ctx, TYPE_INT));
                    env_define(ctx->current_env, btoken, false, ty(ctx, TYPE_INT));
                }
                Token itoken;
                itoken.start = "i"; itoken.length = 1;
                env_define(ctx->current_env, itoken, false, ty(ctx, TYPE_INT));
                check_node(ctx, node->as.stream_gen.gen_expr);
                ctx->current_env = gen_env.parent;
                env_free(&gen_env);
                node->semantic_type = ty(ctx, TYPE_STREAM);
            } else {
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            // Stream generator bounds (while/until) are not yet implemented
            if (node->as.stream_gen.bound) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "bounded stream generators are not yet supported");
            }
            break;
        case AST_ARRAY: {
            if (node->as.array.element_count == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EMPTY_ARRAY,
                           "cannot infer element type for empty array");
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                break;
            }
            SemanticType *elem = NULL;
            for (int i = 0; i < node->as.array.element_count; i++) {
                check_node(ctx, node->as.array.elements[i]);
                SemanticType *et = node->as.array.elements[i] ?
                    node->as.array.elements[i]->semantic_type : NULL;
                SemanticType *u = unify(ctx, node->token.line, elem, et,
                                        "array elements must have the same type");
                if (u) elem = u;
            }
            node->semantic_type = type_get_array(ctx->pool,
                                                 ty(ctx, elem ? elem->kind : TYPE_UNKNOWN),
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
                bool found = false;
                for (int i = 0; i < tt->field_count; i++) {
                    if ((int)node->as.field_access.field.length == (int)strlen(tt->field_names[i]) &&
                        memcmp(node->as.field_access.field.start, tt->field_names[i], node->as.field_access.field.length) == 0) {
                        ft = tt->field_types[i] ? tt->field_types[i] : ty(ctx, TYPE_UNKNOWN);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown field '%.*s'",
                             (int)node->as.field_access.field.length,
                             node->as.field_access.field.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                }
            } else if (tt && tt->kind != TYPE_UNKNOWN) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                           "field access on non-struct type");
            }
            node->semantic_type = ft;
            break;
        }
        case AST_SPAWN:
            // Fail closed: no concurrency runtime exists yet (M7), so spawn
            // must be rejected instead of compiling to a placeholder value.
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "spawn is not supported yet");
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_CHAN:
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "chan is not supported yet");
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_MATCH: {
            // Check for wildcard arm
            bool has_wildcard = false;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                if (node->as.match_expr.arms[i].is_wildcard) {
                    has_wildcard = true;
                    break;
                }
            }
            if (!has_wildcard) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "match must have a wildcard arm ('_ => ...')");
            }
            check_node(ctx, node->as.match_expr.expr);
            SemanticType *result = NULL;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                if (!node->as.match_expr.arms[i].is_wildcard) {
                    check_node(ctx, node->as.match_expr.arms[i].pattern);
                }
                check_node(ctx, node->as.match_expr.arms[i].body);
                AstNode *body = node->as.match_expr.arms[i].body;
                if (body && body->semantic_type) {
                    SemanticType *u = unify(ctx, body->token.line, result,
                                            body->semantic_type,
                                            "match arms must have the same type");
                    if (u) result = u;
                }
            }
            node->semantic_type = ty(ctx, result ? result->kind : TYPE_UNKNOWN);
            break;
        }
        case AST_STRUCT_DEF: {
            // M12.6: Register struct definition
            Token name = node->as.struct_def.name;
            // Check for duplicate struct name
            if (struct_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "duplicate struct definition '%.*s'",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // Resolve field types
            SemanticType **field_types = NULL;
            if (node->as.struct_def.field_count > 0) {
                field_types = malloc(sizeof(SemanticType *) * (size_t)node->as.struct_def.field_count);
                if (!field_types) { fprintf(stderr, "out of memory\n"); exit(1); }
                for (int i = 0; i < node->as.struct_def.field_count; i++) {
                    SemanticType *ft = resolve_type_annot(ctx, node->as.struct_def.field_types[i]);
                    if (!ft) {
                        char msg[128];
                        snprintf(msg, sizeof msg, "unknown field type '%.*s'",
                                 (int)node->as.struct_def.field_types[i].length,
                                 node->as.struct_def.field_types[i].start);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                        ft = ty(ctx, TYPE_UNKNOWN);
                    }
                    field_types[i] = ft;
                }
            }
            // Create and register the struct type
            SemanticType *st = type_get_struct(ctx->pool, name,
                                               node->as.struct_def.field_names,
                                               field_types,
                                               node->as.struct_def.field_count);
            char name_buf[64];
            snprintf(name_buf, sizeof name_buf, "%.*s", (int)name.length, name.start);
            struct_register(ctx, name_buf, st);
            free(field_types);
            node->semantic_type = st;
            break;
        }
        case AST_RECORD_LIT: {
            // M12.6: Check record literal against struct definition
            Token struct_name = node->as.record_lit.struct_name;
            SemanticType *st = struct_lookup(ctx, struct_name);
            if (!st) {
                char msg[128];
                snprintf(msg, sizeof msg, "unknown struct '%.*s'",
                         (int)struct_name.length, struct_name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                break;
            }
            // Check field count matches
            if (node->as.record_lit.field_count != st->field_count) {
                char msg[128];
                snprintf(msg, sizeof msg, "record literal has %d fields, struct has %d",
                         node->as.record_lit.field_count, st->field_count);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // Check each field
            for (int i = 0; i < node->as.record_lit.field_count; i++) {
                Token field_name = node->as.record_lit.field_names[i];
                // Find field in struct
                int field_idx = -1;
                for (int j = 0; j < st->field_count; j++) {
                    if ((int)field_name.length == (int)strlen(st->field_names[j]) &&
                        memcmp(field_name.start, st->field_names[j], field_name.length) == 0) {
                        field_idx = j;
                        break;
                    }
                }
                if (field_idx < 0) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown field '%.*s'",
                             (int)field_name.length, field_name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                    continue;
                }
                // Check field value type
                check_node(ctx, node->as.record_lit.field_values[i]);
                SemanticType *vt = node->as.record_lit.field_values[i] ?
                    (SemanticType *)node->as.record_lit.field_values[i]->semantic_type : NULL;
                SemanticType *expected = st->field_types[field_idx];
                if (vt && expected && vt->kind != TYPE_UNKNOWN && expected->kind != TYPE_UNKNOWN) {
                    unify(ctx, node->token.line, expected, vt, "field type mismatch");
                }
            }
            node->semantic_type = st;
            break;
        }
    }
}

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag, TypePool *pool) {
    SemanticContext ctx;
    ctx.path = path;
    ctx.diag = diag;
    ctx.loop_depth = 0;
    ctx.pool = pool;
    ctx.in_range_context = false;
    ctx.structs = NULL;
    ctx.struct_count = 0;
    ctx.struct_capacity = 0;
    ctx.funcs = NULL;
    ctx.func_count = 0;
    ctx.func_capacity = 0;
    Environment global_env;
    env_init(&global_env, NULL);
    ctx.current_env = &global_env;
    for (int i = 0; i < count; i++) {
        check_node(&ctx, stmts[i]);
    }
    env_free(&global_env);
    // Free struct registry
    for (int i = 0; i < ctx.struct_count; i++) {
        free(ctx.structs[i].name);
    }
    free(ctx.structs);
    // M9.1: free function registry
    for (int i = 0; i < ctx.func_count; i++) {
        free(ctx.funcs[i].name);
    }
    free(ctx.funcs);
}
