// Semantic checker: AST node dispatch (check_node) and program entry
// points. Per-node work is split across env.c / symtab.c / typecheck.c /
// check_builtins.c / check_call.c / check_pattern.c; the internal API is
// include/semantic_int.h.
#include "../include/semantic_int.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void check_node(SemanticContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_LITERAL: {
            PrimitiveType p = TYPE_UNKNOWN;
            if (node->as.literal.type == TOK_INT) {
                p = TYPE_INT;
                // Integer literals default to i64 (LANGUAGE_SPEC §11);
                // out-of-range literals are rejected at compile time.
                // Lexer allows underscore digit separators (1_000); strip them
                // before parsing because strtoll stops at '_'.
                char tmp[64];
                size_t tl = 0;
                for (size_t i = 0; i < node->token.length && tl < sizeof(tmp) - 1; i++)
                    if (node->token.start[i] != '_') tmp[tl++] = node->token.start[i];
                tmp[tl] = '\0';
                errno = 0;
                char *end = NULL;
                (void)strtoll(tmp, &end, 10);
                if (errno == ERANGE || end != tmp + tl) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_LITERAL_RANGE,
                               "integer literal out of range for i64");
                }
            }
            else if (node->as.literal.type == TOK_FLOAT) p = TYPE_FLOAT;
            else if (node->as.literal.type == TOK_STRING) p = TYPE_STR;
            else if (node->as.literal.type == TOK_TRUE || node->as.literal.type == TOK_FALSE) p = TYPE_BOOL;
            else if (node->as.literal.type == TOK_NONE) p = TYPE_OPTION;
            node->semantic_type = ty(ctx, p);
            break;
        }
        case AST_IDENTIFIER: {
            Token id_name = node->as.identifier.name;
            Symbol *sym = env_lookup(ctx->current_env, node->as.identifier.name);
            if (!sym) {
                char msg[256];
                // M13.1-P2: a bare enum name is not a value (§17.5).
                if (enum_lookup(ctx, id_name)) {
                    snprintf(msg, sizeof(msg), "enum '%.*s' is not a value; use %.*s.<variant>",
                             (int)id_name.length, id_name.start,
                             (int)id_name.length, id_name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    break;
                }
                snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                // M15: helpful hint for gated domain builtins.
                if (!ctx->is_std) {
                    const char *p = node->as.identifier.name.start;
                    int nl = (int)node->as.identifier.name.length;
                    const char *hint = NULL;
                    if (nl >= 5 && memcmp(p, "json_", 5) == 0) hint = " \xe2\x80\x94 import \"std/json.tiq\" for JSON operations";
                    else if (nl >= 4 && memcmp(p, "net_", 4) == 0) hint = " \xe2\x80\x94 import \"std/net.tiq\" for networking";
                    else if (nl >= 5 && memcmp(p, "http_", 5) == 0) hint = " \xe2\x80\x94 import \"std/net.tiq\" for HTTP";
                    else if (nl >= 3 && memcmp(p, "ev_", 3) == 0) hint = " \xe2\x80\x94 import \"std/ev.tiq\" for event loop";
                    else if (nl >= 3 && memcmp(p, "dl_", 3) == 0) hint = " \xe2\x80\x94 import \"std/dl.tiq\" for dynamic library loading";
                    if (hint) {
                        size_t base = strlen(msg);
                        snprintf(msg + base, sizeof(msg) - base, "%s", hint);
                    }
                }
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
                        // Pre-M13 S4: unsupported equality must fail closed
                        // instead of inheriting C behaviour (LANGUAGE_SPEC §17.1).
                        // TYPE_UNKNOWN is allowed through (unresolved; not a
                        // non-comparable type).
                        if (u->kind != TYPE_UNKNOWN && !eq_comparable_kind(u->kind)) {
                            char tbuf[64];
                            char msg[200];
                            type_display(u, tbuf, sizeof tbuf);
                            snprintf(msg, sizeof msg,
                                     "type '%s' does not support equality comparison",
                                     tbuf);
                            diag_error(ctx->diag, ctx->path, node->token.line,
                                       ERR_TYPE_MISMATCH, msg);
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        } else {
                            node->semantic_type = ty(ctx, TYPE_BOOL);
                        }
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
            if (node->as.conditional.then_branch) check_node(ctx, node->as.conditional.then_branch);
            if (node->as.conditional.else_branch) check_node(ctx, node->as.conditional.else_branch);
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
                        node->semantic_type = u;
                    } else {
                        node->semantic_type = tt;
                    }
                } else if (tt && !node->as.conditional.else_branch) {
                    // M25: one-arm conditional (`cond ? then`). The condition
                    // is checked for bool above; the then-branch value is
                    // discarded and the conditional's type is unit.
                    node->semantic_type = ty(ctx, TYPE_UNIT);
                } else if (tt) {
                    node->semantic_type = tt;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_CALL:
            check_call(ctx, node);
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
            if (node->as.block.final_expr && node->as.block.final_expr->semantic_type) {
                node->semantic_type = node->as.block.final_expr->semantic_type;
            } else {
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            break;
        }
        case AST_BINDING:
            // M22: reject reserved prelude names on LHS.
            if (is_reserved_name(ctx->current_env, node->as.binding.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is a reserved builtin name",
                         (int)node->as.binding.name.length, node->as.binding.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // M22: reject enum/value namespace collision (Finding 5).
            if (enum_lookup(ctx, node->as.binding.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is already defined as an enum",
                         (int)node->as.binding.name.length, node->as.binding.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            // Issue #6: `name <- expr` (or `name = expr`) never shadows an
            // existing binding. Resolve outward via lexical lookup, then:
            //   <-  and nearest binding mutable   => reassign (rewrite to
            //                                       AST_ASSIGN so the emitter
            //                                       does not redeclare the name)
            //   <-  and nearest binding immutable => E11 (no shadow fallback)
            //   =   and a binding exists          => E11 (redefinition)
            //   no binding anywhere               => declare in current scope
            {
                Symbol *prev = env_lookup(ctx->current_env, node->as.binding.name);
                char msg[128];
                if (prev) {
                    if (!node->as.binding.is_mutable) {
                        snprintf(msg, sizeof msg, "cannot redefine binding '%.*s'",
                                 (int)node->as.binding.name.length, node->as.binding.name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_IMMUTABLE_ASSIGNMENT, msg);
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    }
                    if (!prev->is_mutable) {
                        snprintf(msg, sizeof msg, "cannot mutate immutable binding '%.*s'",
                                 (int)node->as.binding.name.length, node->as.binding.name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_IMMUTABLE_ASSIGNMENT, msg);
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    }
                    Token bname = node->as.binding.name;
                    AstNode *bexpr = node->as.binding.expr;
                    node->kind = AST_ASSIGN;
                    node->as.assign.name = bname;
                    node->as.assign.op = TOK_LARROW;
                    node->as.assign.index = NULL;
                    node->as.assign.expr = bexpr;
                    node->as.assign.is_definition = false;
                    check_node(ctx, node);
                    break;
                }
            }
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
            // M22: reject reserved prelude names on LHS of definitions.
            if (node->as.assign.is_definition && is_reserved_name(ctx->current_env, node->as.assign.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is a reserved builtin name",
                         (int)node->as.assign.name.length, node->as.assign.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
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
        case AST_EXTERN: {
            // M16.1/M16.2: extern "C" declaration (LANGUAGE_SPEC §7.1).
            // The ABI operand must be exactly "C" (the token includes its
            // quotes); every parameter needs an FFI-safe annotation; the
            // name must not collide with an existing declaration.
            Token name = node->as.function.name;
            if (!(node->token.kind == TOK_STRING && node->token.length == 3 &&
                  memcmp(node->token.start, "\"C\"", 3) == 0)) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                           "extern ABI must be \"C\"");
            }
            AstNode *prev = func_lookup(ctx, name);
            if (prev && prev->kind == AST_EXTERN) {
                char msg[160];
                snprintf(msg, sizeof msg, "duplicate extern declaration '%.*s'",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN, msg);
            } else if (prev || struct_lookup(ctx, name) || enum_lookup(ctx, name)) {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "extern declaration '%.*s' collides with an existing declaration",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN, msg);
            }
            for (int i = 0; i < node->as.function.param_count; i++) {
                if (node->as.function.param_ref_kinds &&
                    node->as.function.param_ref_kinds[i] != 0) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW,
                               "extern parameters cannot use borrow annotations");
                }
                Token annot = node->as.function.param_type_annots[i];
                if (annot.kind == TOK_EOF) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                               "extern parameters require type annotations");
                    continue;
                }
                if (annot.kind != TOK_IDENT) {
                    // Compound annotations ([T; N] forms) are not FFI-safe.
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                               "extern parameter type is not FFI-safe");
                    continue;
                }
                Token elem = {0};
                if (node->as.function.param_elem_annots)
                    elem = node->as.function.param_elem_annots[i];
                SemanticType *cont = resolve_container_annot(ctx, annot, elem);
                if (cont) {
                    if (cont->kind != TYPE_UNKNOWN)
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                                   "extern parameter type is not FFI-safe");
                    continue;
                }
                SemanticType *pt = resolve_type_annot(ctx, annot);
                if (!pt) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown type '%.*s'",
                             (int)annot.length, annot.start);
                    diag_error(ctx->diag, ctx->path, annot.line, ERR_TYPE_MISMATCH, msg);
                    continue;
                }
                if (!ffi_safe_kind(pt->kind)) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                               "extern parameter type is not FFI-safe");
                    continue;
                }
                if (node->as.function.param_types)
                    node->as.function.param_types[i] = pt;
            }
            // The return type is mandatory, resolved, and FFI-safe.
            SemanticType *ret = NULL;
            Token ra = node->as.function.return_type_annot;
            if (ra.kind == TOK_IDENT) {
                SemanticType *rcont = resolve_container_annot(ctx, ra,
                    node->as.function.return_elem_annot);
                if (rcont) {
                    if (rcont->kind != TYPE_UNKNOWN)
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                                   "extern return type is not FFI-safe");
                } else {
                    ret = resolve_type_annot(ctx, ra);
                    if (!ret) {
                        char msg[128];
                        snprintf(msg, sizeof msg, "unknown type '%.*s'",
                                 (int)ra.length, ra.start);
                        diag_error(ctx->diag, ctx->path, ra.line, ERR_TYPE_MISMATCH, msg);
                    } else if (!ffi_safe_kind(ret->kind)) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                                   "extern return type is not FFI-safe");
                        ret = NULL;
                    }
                }
            }
            // Register like a user function (body == NULL): calls see the
            // declared signature through the symbol and the func registry.
            PrimitiveType ret_kind = ret ? ret->kind : TYPE_UNKNOWN;
            SemanticType *func_type = (ret && ret->kind == TYPE_STRUCT) ? ret :
                type_get_func(ctx->pool, ret_kind, node->as.function.param_count);
            env_define(ctx->current_env, name, false, func_type);
            func_register(ctx, name, node);
            node->semantic_type = ret ? ret : ty(ctx, TYPE_UNKNOWN);
            break;
        }
        case AST_FUNCTION: {
            // M22: reject reserved prelude names as function names.
            if (is_reserved_name(ctx->current_env, node->as.function.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is a reserved builtin name",
                         (int)node->as.function.name.length, node->as.function.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // M13.4-S3: resolve the return annotation before checking the
            // body so recursive calls see the correct return type.
            PrimitiveType pre_ret = TYPE_UNKNOWN;
            SemanticType *pre_ret_full = NULL;
            if (node->as.function.return_type_annot.kind == TOK_IDENT) {
                SemanticType *ra = resolve_container_annot(ctx,
                    node->as.function.return_type_annot,
                    node->as.function.return_elem_annot);
                if (!ra)
                    ra = resolve_type_annot(ctx, node->as.function.return_type_annot);
                if (ra) {
                    pre_ret = ra->kind;
                    if (ra->kind == TYPE_STRUCT || ra->kind == TYPE_VEC)
                        pre_ret_full = ra;
                }
            }
            SemanticType *func_type = pre_ret_full ? pre_ret_full :
                type_get_func(ctx->pool, pre_ret, node->as.function.param_count);
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
                bool is_container = false;
                // Check for type annotation
                if (node->as.function.param_type_annots &&
                    node->as.function.param_type_annots[i].kind == TOK_IDENT) {
                    // M13.1-P8: container annotations resolve before plain
                    // names (vec/strbuf/map win in annotation position).
                    Token elem_tok = {0};
                    if (node->as.function.param_elem_annots)
                        elem_tok = node->as.function.param_elem_annots[i];
                    SemanticType *cont = resolve_container_annot(ctx,
                        node->as.function.param_type_annots[i], elem_tok);
                    if (cont) {
                        pt = cont;
                        is_container = cont->kind != TYPE_UNKNOWN;
                    } else {
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
                }
                // M9.1: wrap reference parameters as &T / &mut T.
                if (node->as.function.param_ref_kinds &&
                    node->as.function.param_ref_kinds[i] != 0) {
                    if (is_container) {
                        // M13.1-P8: containers are shared handles (§19.10).
                        diag_error(ctx->diag, ctx->path,
                                   node->as.function.param_type_annots[i].line, ERR_BORROW,
                                   "container parameters are reference-semantics handles; '&' is not allowed");
                    } else {
                        pt = type_get_ref(ctx->pool, pt,
                                          node->as.function.param_ref_kinds[i] == 2);
                    }
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
                // M13.1-P8: container return annotations resolve first.
                SemanticType *ret_annot = resolve_container_annot(ctx,
                    node->as.function.return_type_annot,
                    node->as.function.return_elem_annot);
                if (!ret_annot)
                    ret_annot = resolve_type_annot(ctx, node->as.function.return_type_annot);
                if (ret_annot) {
                    SemanticType *u = unify(ctx, node->token.line, ret_annot,
                                            ty(ctx, ret_kind), "return type mismatch");
                    if (u) ret_kind = u->kind;
                    // M12.6: Preserve full struct type for return
                    // M13.1-P8: likewise vec<T>, so callers get the element type
                    if (ret_annot->kind == TYPE_STRUCT || ret_annot->kind == TYPE_VEC)
                        ret_type = ret_annot;
                    // M13.1-P8: nominal element check for vec expression
                    // bodies (block bodies are kind-checked only, §19.10).
                    if (ret_annot->kind == TYPE_VEC && ret_annot->element_type &&
                        node->as.function.body && node->as.function.body->semantic_type) {
                        SemanticType *bt = (SemanticType *)node->as.function.body->semantic_type;
                        if (bt->kind == TYPE_VEC && bt->element_type &&
                            !vec_elem_same(ret_annot->element_type, bt->element_type)) {
                            char want[96], got[96], msg[320];
                            type_display(ret_annot, want, sizeof want);
                            type_display(bt, got, sizeof got);
                            snprintf(msg, sizeof msg, "return type mismatch: expected %s, found %s",
                                     want, got);
                            diag_error(ctx->diag, ctx->path, node->token.line,
                                       ERR_TYPE_MISMATCH, msg);
                        }
                    }
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
                if (pre_ret != TYPE_UNKNOWN) {
                    // M13.4-S3: annotation was pre-registered; the annotation
                    // check above already reported any mismatch.  Just ensure
                    // the symbol carries the final resolved type.
                    if (ret_type && (ret_type->kind == TYPE_STRUCT || ret_type->kind == TYPE_VEC)) {
                        sym->type = ret_type;
                    } else {
                        sym->type = type_get_func(ctx->pool, pre_ret, node->as.function.param_count);
                    }
                } else {
                    // Unify the previously recorded return type (unknown for a
                    // fresh definition) with the body type before re-pointing.
                    SemanticType *u = unify(ctx, node->token.line,
                                            ty(ctx, sym->type ? sym->type->kind : TYPE_UNKNOWN),
                                            ty(ctx, ret_kind), "type mismatch");
                    if (u && u->kind != TYPE_UNKNOWN) {
                        // M12.6: For struct returns, use the full struct type so
                        // callers can access fields. param_count is lost but
                        // arity is checked at the definition site.
                        // M13.1-P8: vec returns keep the full vec<T> the same way
                        // (arity comes from the recorded definition at call sites).
                        // Pre-M13 S4: Option/Result returns preserve inner and
                        // error types so callers see the full parameterised type.
                        if (ret_type && (ret_type->kind == TYPE_STRUCT || ret_type->kind == TYPE_VEC)) {
                            sym->type = ret_type;
                        } else if ((ret_kind == TYPE_OPTION || ret_kind == TYPE_RESULT)
                                   && node->as.function.body
                                   && node->as.function.body->semantic_type) {
                            SemanticType *bt = (SemanticType *)node->as.function.body->semantic_type;
                            SemanticType *ft = calloc(1, sizeof(SemanticType));
                            if (!ft) die_oom();
                            ft->kind = (PrimitiveType)ret_kind;
                            ft->inner_type = bt->inner_type;
                            ft->error_type = bt->error_type;
                            ft->param_count = node->as.function.param_count;
                            sym->type = ft;
                        } else {
                            sym->type = type_get_func(ctx->pool, u->kind, node->as.function.param_count);
                        }
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
            // M22: bare range loops require an explicit binder.
            if (is_range && !node->as.bracket_loop.has_binder) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                           "range loop requires an explicit binder: use [name <- domain]");
            }
            Environment loop_env;
            env_init(&loop_env, ctx->current_env);
            ctx->current_env = &loop_env;
            if (node->as.bracket_loop.has_binder) {
                // The binder replaces the implicit index; loop variables
                // are immutable inside the body.
                env_define(ctx->current_env, node->as.bracket_loop.binder, false,
                           ty(ctx, TYPE_INT));
            }
            // M22: bare range loops do NOT inject an implicit 'i'.
            // Users must write [i <- 0..10] { ... } for an index variable.
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
                // M22: require explicit binders for generator expressions.
                if (node->as.stream_gen.binder_count == 0) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                               "stream generators require explicit binders: use (name) -> expr");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    break;
                }
                // Binder count must match seed count.
                if (node->as.stream_gen.binder_count != node->as.stream_gen.seed_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "binder count must match seed count");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    break;
                }
                // Check for duplicate binder names.
                for (int b = 0; b < node->as.stream_gen.binder_count; b++) {
                    for (int b2 = b + 1; b2 < node->as.stream_gen.binder_count; b2++) {
                        if (node->as.stream_gen.binders[b].length == node->as.stream_gen.binders[b2].length &&
                            memcmp(node->as.stream_gen.binders[b].start, node->as.stream_gen.binders[b2].start,
                                   node->as.stream_gen.binders[b].length) == 0) {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                                       "duplicate stream binder name");
                        }
                    }
                    if (node->as.stream_gen.has_index_binder &&
                        node->as.stream_gen.binders[b].length == node->as.stream_gen.index_binder.length &&
                        memcmp(node->as.stream_gen.binders[b].start, node->as.stream_gen.index_binder.start,
                               node->as.stream_gen.binders[b].length) == 0) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                                   "duplicate stream binder name");
                    }
                }
                Environment gen_env;
                env_init(&gen_env, ctx->current_env);
                ctx->current_env = &gen_env;
                // Define explicit window binders.
                for (int b = 0; b < node->as.stream_gen.binder_count; b++) {
                    env_define(ctx->current_env, node->as.stream_gen.binders[b], false, ty(ctx, TYPE_INT));
                }
                // Define optional explicit index binder.
                if (node->as.stream_gen.has_index_binder) {
                    env_define(ctx->current_env, node->as.stream_gen.index_binder, false, ty(ctx, TYPE_INT));
                }
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
                // Strip underscore digit separators before parsing (lexer allows 1_000).
                const char *ls = node->as.array_fill.length->token.start;
                size_t ll = node->as.array_fill.length->token.length;
                char ltmp[64];
                size_t tl = 0;
                for (size_t i = 0; i < ll && tl < sizeof(ltmp) - 1; i++)
                    if (ls[i] != '_') ltmp[tl++] = ls[i];
                ltmp[tl] = '\0';
                errno = 0;
                char *lend = NULL;
                long long lv = strtoll(ltmp, &lend, 10);
                if (errno == ERANGE || lv < 0 || lv > 1000000) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_LITERAL_RANGE,
                               "array fill length out of range");
                } else {
                    fill_len = (int)lv;
                }
            }
            node->semantic_type = type_get_array(ctx->pool, ty(ctx, vt ? vt->kind : TYPE_UNKNOWN),
                                                 fill_len);
            break;
        }
        case AST_FIELD_ACCESS: {
            // M13.1-P2: Name.Variant resolves through the enum registry first;
            // an identifier target naming a declared enum wins over any
            // same-named value binding (LANGUAGE_SPEC §17.5).
            AstNode *fa_target = node->as.field_access.target;
            if (fa_target && fa_target->kind == AST_IDENTIFIER) {
                EnumEntry *ee = enum_lookup(ctx, fa_target->as.identifier.name);
                if (ee) {
                    if (enum_variant_index(ee, node->as.field_access.field) < 0) {
                        char msg[160];
                        snprintf(msg, sizeof msg, "unknown variant '%.*s' in enum '%s'",
                                 (int)node->as.field_access.field.length,
                                 node->as.field_access.field.start, ee->name);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNKNOWN_VARIANT, msg);
                    }
                    fa_target->semantic_type = ty(ctx, TYPE_INT);
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
            }
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
        case AST_IMPORT:
            // M13.1-P6: imports are resolved by the module loader before
            // checking begins; a node reaching here (single-file debug
            // paths like dump-typed-ast) is inert.
            break;
        case AST_MATCH: {
            // Pre-M13 S1: every irrefutable arm (`_` wildcard or bare
            // binding) must be the final arm. Earlier irrefutable arms
            // make every later arm unreachable and are rejected with
            // E07 before code generation.
            for (int i = 0; i < node->as.match_expr.arm_count - 1; i++) {
                Pattern *p = node->as.match_expr.arms[i].pat;
                if (p && (p->kind == PAT_WILDCARD || p->kind == PAT_BINDING)) {
                    diag_error(ctx->diag, ctx->path, p->token.line,
                               ERR_UNSUPPORTED_STATEMENT,
                               "irrefutable pattern must be the last arm");
                    break;
                }
            }
            // Wildcard requirement preserved for compatibility with the
            // M12.7.2 contract — every match must end with at least one
            // wildcard, so the irrefutable-last rule above subsumes it
            // (an early wildcard is also caught).  When the last arm is
            // a bare binding (no wildcard follows) we still require an
            // explicit `_` somewhere — keep the legacy message.
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
            SemanticType *scrut_type = node->as.match_expr.expr ?
                node->as.match_expr.expr->semantic_type : NULL;
            SemanticType *result = NULL;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                // Each arm gets its own scope for pattern bindings
                Environment arm_env;
                env_init(&arm_env, ctx->current_env);
                ctx->current_env = &arm_env;
                check_pattern(ctx, node->as.match_expr.arms[i].pat, scrut_type, &arm_env);
                check_node(ctx, node->as.match_expr.arms[i].body);
                ctx->current_env = arm_env.parent;
                env_free(&arm_env);
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
            // M13.1-P2: struct and enum names share a namespace (§17.5).
            if (enum_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "struct '%.*s' conflicts with enum '%.*s'",
                         (int)name.length, name.start, (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
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
        case AST_ENUM_DEF: {
            // M13.1-P2: register the enum; variants keep declaration order so
            // their values (indices) and the emitted constants are
            // deterministic (LANGUAGE_SPEC §17.5).
            Token name = node->as.enum_def.name;
            if (enum_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "duplicate enum definition '%.*s'",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            if (struct_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "enum '%.*s' conflicts with struct '%.*s'",
                         (int)name.length, name.start, (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            // M22: check if enum name collides with an existing value binding (Finding 5).
            {
                Symbol *vsym = env_lookup(ctx->current_env, name);
                if (vsym && !vsym->is_reserved) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "'%.*s' is already defined as a value",
                             (int)name.length, name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
                }
            }
            for (int i = 0; i < node->as.enum_def.variant_count; i++) {
                for (int j = 0; j < i; j++) {
                    Token a = node->as.enum_def.variants[i];
                    Token b = node->as.enum_def.variants[j];
                    if (a.length == b.length && memcmp(a.start, b.start, a.length) == 0) {
                        char msg[160];
                        snprintf(msg, sizeof msg, "duplicate variant '%.*s' in enum '%.*s'",
                                 (int)a.length, a.start, (int)name.length, name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_VARIANT, msg);
                        break;
                    }
                }
                // M22: check if variant name collides with a value binding
                // already defined in the current scope (Finding 5).
                Token v = node->as.enum_def.variants[i];
                Symbol *vsym = env_lookup(ctx->current_env, v);
                if (vsym && !vsym->is_reserved) {
                    char msg[160];
                    snprintf(msg, sizeof msg,
                             "enum variant '%.*s' shadows existing binding",
                             (int)v.length, v.start);
                    diag_error(ctx->diag, ctx->path, node->token.line,
                               ERR_DUPLICATE_VARIANT, msg);
                }
            }
            enum_register(ctx, name, node->as.enum_def.variants, node->as.enum_def.variant_count);
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
    SemanticModule mod = { stmts, count, path };
    semantic_check_modules(&mod, 1, diag, pool);
}

// M13.1-P6: check a multi-file program as one flat global namespace: the
// modules arrive in dependency post-order and share a single environment
// and struct/enum/function registry set, so cross-module duplicates hit
// the same diagnostics as duplicates within one file (§17.6). Only
// ctx.path switches per module so diagnostics name the right file.
void semantic_check_modules(SemanticModule *mods, int mod_count, DiagContext *diag, TypePool *pool) {
    SemanticContext ctx;
    ctx.path = mod_count > 0 ? mods[0].path : "<none>";
    ctx.diag = diag;
    ctx.loop_depth = 0;
    ctx.pool = pool;
    ctx.in_range_context = false;
    ctx.is_std = false;
    ctx.structs = NULL;
    ctx.struct_count = 0;
    ctx.struct_capacity = 0;
    ctx.funcs = NULL;
    ctx.func_count = 0;
    ctx.func_capacity = 0;
    ctx.enums = NULL;
    ctx.enum_count = 0;
    ctx.enum_capacity = 0;
    Environment global_env;
    env_init(&global_env, NULL);
    ctx.current_env = &global_env;
    // M22: install reserved prelude builtins.  These names cannot be
    // redefined by user code (reserved-prelude policy).
    {
        static const char *prelude_names[] = {
            "print", "len", "some", "ok", "err",
            "cli_arg_count", "cli_arg",
            "vec_push", "vec_pop", "vec_len", "vec_get", "vec_set",
            "map_get", "map_set", "map_has", "map_len", "map_remove",
            "str_buf_push", "str_buf_len", "str_buf_get", "str_buf_clear",
            NULL
        };
        for (int pi = 0; prelude_names[pi]; pi++) {
            Token pt;
            pt.start = prelude_names[pi];
            pt.length = (int)strlen(prelude_names[pi]);
            pt.line = 0;
            pt.kind = TOK_IDENT;
            pt.comment_start = NULL;
            pt.comment_length = 0;
            env_define_reserved(&global_env, pt, ty(&ctx, TYPE_INT));
        }
    }
    for (int m = 0; m < mod_count; m++) {
        ctx.path = mods[m].path;
        // M15: detect std/ library modules by path prefix.
        const char *p = mods[m].path;
        ctx.is_std = (strncmp(p, "std/", 4) == 0) ||
                     (p[0] == '/' && strstr(p, "/std/") != NULL) ||
                     (strstr(p, "./std/") != NULL);
        for (int i = 0; i < mods[m].count; i++) {
            check_node(&ctx, mods[m].stmts[i]);
        }
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
    // M13.1-P2: free enum registry
    for (int i = 0; i < ctx.enum_count; i++) {
        for (int j = 0; j < ctx.enums[i].variant_count; j++) {
            free(ctx.enums[i].variants[j]);
        }
        free(ctx.enums[i].variants);
        free(ctx.enums[i].name);
    }
    free(ctx.enums);
}
