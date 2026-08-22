// AST_CALL semantic checking: builtins, conversions, Option/Result
// constructors, borrow arguments, slice/stream indexing, and general
// function calls. Split from the monolithic src/semantic.c.
#include "../include/semantic_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void check_call(SemanticContext *ctx, AstNode *node) {
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
                    return;
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
                    return;
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
                    return;
                }
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
                    return;
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
                    return;
                }
                // M13.1-P3: Vec builtins (LANGUAGE_SPEC §19.7). vec_push and
                // vec_set have heterogeneous signatures the table below
                // cannot express, so each is checked in check_vec_builtin.
                if ((name.length == 7 && memcmp(name.start, "vec_new", 7) == 0) ||
                    (name.length == 18 && memcmp(name.start, "vec_with_allocator", 18) == 0) ||
                    (name.length == 8 && memcmp(name.start, "vec_push", 8) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_get", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_set", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_len", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_pop", 7) == 0)) {
                    check_vec_builtin(ctx, node, name);
                    return;
                }
                // M13.1-P4: StrBuf builtins (LANGUAGE_SPEC §19.8).
                // str_buf_append's (strbuf, str) signature does not fit the
                // table below, so all go through check_strbuf_builtin.
                if ((name.length == 11 && memcmp(name.start, "str_buf_new", 11) == 0) ||
                    (name.length == 22 && memcmp(name.start, "str_buf_with_allocator", 22) == 0) ||
                    (name.length == 14 && memcmp(name.start, "str_buf_append", 14) == 0) ||
                    (name.length == 14 && memcmp(name.start, "str_buf_to_str", 14) == 0) ||
                    (name.length == 11 && memcmp(name.start, "str_buf_len", 11) == 0)) {
                    check_strbuf_builtin(ctx, node, name);
                    return;
                }
                // M13.1-P5: Map builtins (LANGUAGE_SPEC §19.9). map_set's
                // (map, str, int) signature does not fit the table below,
                // so all go through check_map_builtin.
                if ((name.length == 7 && memcmp(name.start, "map_new", 7) == 0) ||
                    (name.length == 18 && memcmp(name.start, "map_with_allocator", 18) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_set", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_get", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_has", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_len", 7) == 0) ||
                    (name.length == 10 && memcmp(name.start, "map_key_at", 10) == 0) ||
                    (name.length == 10 && memcmp(name.start, "map_val_at", 10) == 0)) {
                    check_map_builtin(ctx, node, name);
                    return;
                }
                {
                    typedef struct {
                        const char *name; int name_len; int arity;
                        PrimitiveType expected; PrimitiveType ret;
                        // M10.3: optional distinct type for the second and
                        // later arguments; TYPE_UNKNOWN (0) means "same as
                        // expected".
                        PrimitiveType expected2;
                        // M15: gated builtins require import "std/<mod>.tiq".
                        bool gated;
                    } Builtin;
                    static const Builtin builtins[] = {
                        // Auxiliary Standard Library Primitives
                        // (Isolated stubs; under Milestone M19, these will be rewritten
                        // natively in Tiq language (`std/*.tiq`) using C FFI interop).
                        {"fs_read",        7, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, false},
                        {"fs_write",       8, 2, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, false},
                        {"fs_exists",      9, 1, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN, false},
                        {"proc_exec",      9, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, false},
                        {"proc_exit",      9, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        {"json_parse_int",14, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, true},
                        {"json_encode_str",15,1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.2: JSON decoder builtin (LANGUAGE_SPEC §19).
                        {"json_get",       8, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.3: JSON array builtins (LANGUAGE_SPEC §19).
                        {"json_arr_len",  12, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, true},
                        {"json_arr_get",  12, 2, TYPE_STR, TYPE_STR, TYPE_INT, true},
                        // M10.6: zero-copy JSON member view (LANGUAGE_SPEC §19.1).
                        // M15: stays ungated — its str_view (TiqSlice) result has
                        // no function return annotation, so it cannot be wrapped.
                        {"json_view",      9, 2, TYPE_STR, TYPE_STR_VIEW, TYPE_UNKNOWN, false},
                        // M10.7: JSON key-existence check (LANGUAGE_SPEC §19.1).
                        {"json_has",       8, 2, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN, true},
                        // M10.11: JSON object encoder (LANGUAGE_SPEC §19.1).
                        {"json_set",       8, 3, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        {"json_del",       8, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.13: String utilities (LANGUAGE_SPEC §19.5).
                        {"str_cat",        7, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, false},
                        {"int_str",        7, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, false},
                        // M13.1-P1: substring, byte equality, stderr print,
                        // directory listing (LANGUAGE_SPEC §12, §19.5, §19.6).
                        {"str_sub",        7, 3, TYPE_STR, TYPE_STR, TYPE_INT, false},
                        // M13.4-S3: byte value at index for self-hosted checker.
                        {"str_sub_code",  12, 2, TYPE_STR, TYPE_INT, TYPE_INT, false},
                        {"str_eq",         6, 2, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN, false},
                        {"eprint",         6, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, false},
                        {"fs_list",        7, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, false},
                        {"net_fetch",      9, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.8: TCP socket primitives (LANGUAGE_SPEC §19.3).
                        {"net_listen",    10, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_accept",    10, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_connect",   11, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_recv",       8, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, true},
                        {"net_send",       8, 2, TYPE_INT, TYPE_INT, TYPE_STR, true},
                        {"net_close",      9, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_port",       8, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_shutdown",  12, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        // M10.9: HTTP request-line parsing (LANGUAGE_SPEC §19.3).
                        {"http_method",  11, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        {"http_path",     9, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        {"http_header",  11, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.10: Event loop / kqueue (LANGUAGE_SPEC §19.4).
                        // M15: ev_loop stays ungated — zero-parameter functions
                        // cannot be defined in Tiq, so it cannot be wrapped.
                        {"ev_loop",       7, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        {"ev_add",        6, 2, TYPE_INT, TYPE_INT, TYPE_INT, true},
                        {"ev_wait",       7, 2, TYPE_INT, TYPE_INT, TYPE_INT, true},
                        {"ev_ready",      8, 2, TYPE_INT, TYPE_INT, TYPE_INT, true},
                        // M10.1: CLI argument builtins (LANGUAGE_SPEC §18.1)
                        {"cli_arg_count", 13, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        {"cli_arg",        7, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, false},
                        // M14.3: monotonic millisecond clock (LANGUAGE_SPEC §19.6).
                        {"clock_ms",       8, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        // M16.4: dynamic library loading (LANGUAGE_SPEC §19.11).
                        // dl_error stays ungated — zero-parameter functions
                        // cannot be defined in Tiq, so it cannot be wrapped
                        // (same as ev_loop).
                        {"dl_open",        7, 1, TYPE_STR, TYPE_U64, TYPE_UNKNOWN, true},
                        {"dl_sym",         6, 2, TYPE_U64, TYPE_U64, TYPE_STR, true},
                        {"dl_error",       8, 0, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, false},
                        {"dl_call",        7, 7, TYPE_U64, TYPE_INT, TYPE_INT, true},
                        // M19.2: Thread-safe channel builtins (LANGUAGE_SPEC §19.12).
                        {"chan_new",       8, 1, TYPE_INT, TYPE_U64, TYPE_UNKNOWN, true},
                        {"chan_send",      9, 2, TYPE_U64, TYPE_INT, TYPE_INT, true},
                        {"chan_recv",      9, 1, TYPE_U64, TYPE_INT, TYPE_UNKNOWN, true},
                        {"chan_try_recv", 13, 1, TYPE_U64, TYPE_INT, TYPE_UNKNOWN, true},
                        {"chan_close",    10, 1, TYPE_U64, TYPE_INT, TYPE_UNKNOWN, true},
                        {"chan_len",       8, 1, TYPE_U64, TYPE_INT, TYPE_UNKNOWN, true},
                        {"chan_cap",       8, 1, TYPE_U64, TYPE_INT, TYPE_UNKNOWN, true},
                        {"chan_free",      9, 1, TYPE_U64, TYPE_INT, TYPE_UNKNOWN, true},
                    };
                    bool matched = false;
                    for (int bi = 0; bi < (int)(sizeof builtins / sizeof builtins[0]); bi++) {
                        if ((int)name.length != builtins[bi].name_len ||
                            memcmp(name.start, builtins[bi].name, name.length) != 0)
                            continue;
                        // M15: gate domain builtins behind std/ module import.
                        // Skip the builtin so the call resolves to the imported
                        // wrapper function instead.
                        if (builtins[bi].gated && !ctx->is_std) {
                            continue;
                        }
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
                                if (ai >= 1 && builtins[bi].expected2 != TYPE_UNKNOWN)
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
                    if (matched) return;
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
                        return;
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
                        return;
                    }
                    node->semantic_type = ty(ctx, tgt);
                    return;
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
                            return;
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
                            return;
                        } else if (callee_type->kind == TYPE_STREAM) {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot range-slice a stream generator");
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                            return;
                        } else {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot slice non-array");
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                            return;
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
                        return;
                    } else if (callee_type->kind == TYPE_STR || callee_type->kind == TYPE_STR_VIEW) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "string index must be int");
                        }
                        node->semantic_type = ty(ctx, TYPE_INT);
                        return;
                    } else if (callee_type->kind == TYPE_STREAM) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "stream index must be int");
                        }
                        node->semantic_type = ty(ctx, TYPE_INT);
                        return;
                    }
                }
                // M12.6: Skip arity check for struct-returning functions
                // (param_count is 0 for struct types; arity checked at definition)
                // M13.1-P8: likewise for vec-returning functions — their callee
                // type is the interned vec<T> (param_count 0); arity is checked
                // against the recorded definition below.
                if (!node->as.call.is_bracket_call && callee_type->kind != TYPE_STRUCT &&
                    callee_type->kind != TYPE_VEC &&
                    (callee_type->param_count > 0 ||
                     (callee_type->param_count == 0 && callee_type->kind != TYPE_UNKNOWN)) &&
                    callee_type->param_count != node->as.call.arg_count) {
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
                // M13.1-P8: vec-returning functions carry vec<T> as their
                // callee type, so their arity comes from the definition.
                if (fn_def && node->as.call.callee->semantic_type &&
                    ((SemanticType *)node->as.call.callee->semantic_type)->kind == TYPE_VEC &&
                    fn_def->as.function.param_count != node->as.call.arg_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "arity mismatch");
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
                        // M16.2: extern calls check argument types against
                        // the declared C ABI signature (LANGUAGE_SPEC §7.1);
                        // containers never reach here (rejected at the decl).
                        if (fn_def && fn_def->kind == AST_EXTERN &&
                            i < fn_def->as.function.param_count &&
                            fn_def->as.function.param_types &&
                            fn_def->as.function.param_types[i]) {
                            char where[64];
                            snprintf(where, sizeof where, "argument %d", i + 1);
                            unify(ctx, node->token.line,
                                  (SemanticType *)fn_def->as.function.param_types[i],
                                  arg->semantic_type, where);
                        }
                        if (want_ref != 0) {
                            char msg[128];
                            snprintf(msg, sizeof msg, "argument %d must be borrowed with %s",
                                     i + 1, want_ref == 2 ? "&mut" : "&");
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                        // M13.1-P8: container parameters (§19.10) — the kind
                        // must match, vec elements match nominally, and an
                        // annotated vec[T] param establishes an unestablished
                        // argument vec exactly like a first vec_push would.
                        SemanticType *cpt = NULL;
                        if (fn_def && i < fn_def->as.function.param_count &&
                            fn_def->as.function.param_types)
                            cpt = fn_def->as.function.param_types[i];
                        if (cpt && (cpt->kind == TYPE_VEC || cpt->kind == TYPE_STRBUF ||
                                    cpt->kind == TYPE_MAP)) {
                            char where[64];
                            snprintf(where, sizeof where, "argument %d", i + 1);
                            SemanticType *at = arg->semantic_type;
                            if (unify(ctx, node->token.line, cpt, at, where) && at &&
                                cpt->kind == TYPE_VEC && at->kind == TYPE_VEC) {
                                if (at->element_type && cpt->element_type &&
                                    !vec_elem_same(cpt->element_type, at->element_type)) {
                                    char want[96], got[96], msg[320];
                                    type_display(cpt, want, sizeof want);
                                    type_display(at, got, sizeof got);
                                    snprintf(msg, sizeof msg, "%s: expected %s, found %s",
                                             where, want, got);
                                    diag_error(ctx->diag, ctx->path, node->token.line,
                                               ERR_TYPE_MISMATCH, msg);
                                } else if (!at->element_type && cpt->element_type) {
                                    // Establish: repoint the caller's binding.
                                    if (arg->kind == AST_IDENTIFIER) {
                                        Symbol *asym = env_lookup(ctx->current_env,
                                                                  arg->as.identifier.name);
                                        if (asym && asym->type == at) asym->type = cpt;
                                    }
                                    arg->semantic_type = cpt;
                                }
                            }
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
}
