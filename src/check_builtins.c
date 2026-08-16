// Container (vec / strbuf / map) builtin argument checking for the
// semantic checker. Split from the monolithic src/semantic.c.
#include "../include/semantic_int.h"
#include <stdio.h>
#include <string.h>

// M13.1-P3: Vec builtins (LANGUAGE_SPEC §19.7). Element type T must be
// int, str, or a named struct.
bool vec_elem_type_ok(SemanticType *t) {
    return t && (t->kind == TYPE_INT || t->kind == TYPE_STR ||
                 (t->kind == TYPE_STRUCT && t->struct_name));
}

// M13.1-P3: Element unification is stricter than unify(): struct elements
// must be the same pooled struct type (nominal), not just kind-equal.
// M13.1-P9: the comparison is structural, never pointer identity —
// type_get_func interns function return types per-arity, so an int
// returned by a user function is a distinct pooled TYPE_INT instance from
// the canonical one. Primitives compare by kind; named structs stay
// nominal (struct types are pointer-unique per name in the pool).
bool vec_elem_same(SemanticType *expected, SemanticType *found) {
    return expected->kind == found->kind &&
           (expected->kind != TYPE_STRUCT || expected == found);
}

void vec_unify_elem(SemanticContext *ctx, int line, SemanticType *expected,
                           SemanticType *found, const char *context) {
    if (!found || found->kind == TYPE_UNKNOWN) return;
    if (vec_elem_same(expected, found)) return;
    char want[96];
    char got[96];
    char msg[320];
    type_display(expected, want, sizeof want);
    type_display(found, got, sizeof got);
    snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
    diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
}

// M13.1-P8: resolve container annotations (vec[T] / strbuf / map) in
// function parameter and return position (LANGUAGE_SPEC §19.10).
// Returns NULL when the token is not a container name; malformed vec
// annotations report E09 here and yield TYPE_UNKNOWN so the caller
// does not double-report.
SemanticType *resolve_container_annot(SemanticContext *ctx, Token tok, Token elem_tok) {
    if (tok.kind != TOK_IDENT) return NULL;
    if (tok.length == 6 && memcmp(tok.start, "strbuf", 6) == 0)
        return ty(ctx, TYPE_STRBUF);
    if (tok.length == 3 && memcmp(tok.start, "map", 3) == 0)
        return ty(ctx, TYPE_MAP);
    if (tok.length == 3 && memcmp(tok.start, "vec", 3) == 0) {
        if (elem_tok.kind != TOK_IDENT) {
            diag_error(ctx->diag, ctx->path, tok.line, ERR_TYPE_MISMATCH,
                       "vec annotation requires an element type: vec[T]");
            return ty(ctx, TYPE_UNKNOWN);
        }
        SemanticType *et = resolve_type_annot(ctx, elem_tok);
        if (!et) {
            char msg[128];
            snprintf(msg, sizeof msg, "unknown type '%.*s'",
                     (int)elem_tok.length, elem_tok.start);
            diag_error(ctx->diag, ctx->path, elem_tok.line, ERR_TYPE_MISMATCH, msg);
            return ty(ctx, TYPE_UNKNOWN);
        }
        if (!vec_elem_type_ok(et)) {
            diag_error(ctx->diag, ctx->path, elem_tok.line, ERR_TYPE_MISMATCH,
                       "vec element type must be int, str, or a struct");
            return ty(ctx, TYPE_UNKNOWN);
        }
        return type_get_vec(ctx->pool, et);
    }
    return NULL;
}

// M13.1-P3: Per-builtin checker for the six vec builtins. vec_push and
// vec_set have heterogeneous signatures ((vec, T) / (vec, int, T)) that the
// generic Builtin table in AST_CALL cannot express, so each is handled
// explicitly here. The element type is established by the first vec_push on
// a binding; vec_get/vec_set/vec_pop on a vec with no established element
// type is a fail-closed E09 error (LANGUAGE_SPEC §19.7).
void check_vec_builtin(SemanticContext *ctx, AstNode *node, Token name) {
    char nbuf[32];
    int nlen = (int)name.length < 31 ? (int)name.length : 31;
    memcpy(nbuf, name.start, (size_t)nlen);
    nbuf[nlen] = '\0';
    bool is_new = strcmp(nbuf, "vec_new") == 0;
    bool is_with_alloc = strcmp(nbuf, "vec_with_allocator") == 0;
    bool is_push = strcmp(nbuf, "vec_push") == 0;
    bool is_get = strcmp(nbuf, "vec_get") == 0;
    bool is_set = strcmp(nbuf, "vec_set") == 0;
    bool is_len = strcmp(nbuf, "vec_len") == 0;
    bool is_pop = strcmp(nbuf, "vec_pop") == 0;
    int arity = is_new ? 0 : (is_with_alloc || is_len || is_pop) ? 1 : (is_push || is_get) ? 2 : 3;
    for (int ai = 0; ai < node->as.call.arg_count; ai++) {
        if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
    }
    if (node->as.call.arg_count != arity) {
        char msg[128];
        if (arity == 0) {
            snprintf(msg, sizeof msg, "%s expects no arguments", nbuf);
        } else {
            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                     nbuf, arity, arity == 1 ? "" : "s");
        }
        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
        node->semantic_type = (is_new || is_with_alloc) ? type_get_vec(ctx->pool, NULL)
                                                         : ty(ctx, (is_get || is_pop) ? TYPE_UNKNOWN : TYPE_INT);
        return;
    }
    if (is_new) {
        node->semantic_type = type_get_vec(ctx->pool, NULL);
        return;
    }
    if (is_with_alloc) {
        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), at, "vec_with_allocator argument");
        node->semantic_type = type_get_vec(ctx->pool, NULL);
        return;
    }
    SemanticType *vt = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
    {
        char where[32];
        snprintf(where, sizeof where, "%s argument", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_VEC), vt, where);
    }
    if (is_get || is_set) {
        SemanticType *it = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        char where[32];
        snprintf(where, sizeof where, "%s index", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), it, where);
    }
    SemanticType *elem = (vt && vt->kind == TYPE_VEC) ? vt->element_type : NULL;
    if (is_push) {
        SemanticType *et = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        if (!elem) {
            if (et && et->kind != TYPE_UNKNOWN) {
                if (!vec_elem_type_ok(et)) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "vec_push element must be int, str, or a struct");
                } else if (vt && vt->kind == TYPE_VEC) {
                    // First push establishes T: repoint the binding's symbol
                    // at the interned vec<T> (pooled types are immutable).
                    SemanticType *nv = type_get_vec(ctx->pool, et);
                    if (node->as.call.args[0]->kind == AST_IDENTIFIER) {
                        Symbol *sym = env_lookup(ctx->current_env,
                                                 node->as.call.args[0]->as.identifier.name);
                        if (sym && sym->type == vt) sym->type = nv;
                    }
                    node->as.call.args[0]->semantic_type = nv;
                }
            }
        } else {
            vec_unify_elem(ctx, node->token.line, elem, et, "vec_push element");
        }
        node->semantic_type = ty(ctx, TYPE_INT);
        return;
    }
    if (is_len) {
        node->semantic_type = ty(ctx, TYPE_INT);
        return;
    }
    if (!elem) {
        // Fail closed: no element type was ever established for this vec.
        if (!vt || vt->kind == TYPE_VEC || vt->kind == TYPE_UNKNOWN) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "%s on a vec with no established element type (no vec_push yet)", nbuf);
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
        }
        node->semantic_type = ty(ctx, is_set ? TYPE_INT : TYPE_UNKNOWN);
        return;
    }
    if (is_set) {
        SemanticType *et = node->as.call.args[2] ? node->as.call.args[2]->semantic_type : NULL;
        vec_unify_elem(ctx, node->token.line, elem, et, "vec_set element");
        node->semantic_type = ty(ctx, TYPE_INT);
        return;
    }
    node->semantic_type = elem; // vec_get / vec_pop return T
}

// M13.1-P4: Per-builtin checker for the StrBuf builtins (LANGUAGE_SPEC
// §19.8). str_buf_append's (strbuf, str) signature does not fit the generic
// Builtin table in AST_CALL, so all are handled explicitly here, cloned
// from check_vec_builtin. TYPE_STRBUF is not parametrized, so there is no
// element-type establishment step.
void check_strbuf_builtin(SemanticContext *ctx, AstNode *node, Token name) {
    char nbuf[32];
    int nlen = (int)name.length < 31 ? (int)name.length : 31;
    memcpy(nbuf, name.start, (size_t)nlen);
    nbuf[nlen] = '\0';
    bool is_new = strcmp(nbuf, "str_buf_new") == 0;
    bool is_with_alloc = strcmp(nbuf, "str_buf_with_allocator") == 0;
    bool is_append = strcmp(nbuf, "str_buf_append") == 0;
    bool is_to_str = strcmp(nbuf, "str_buf_to_str") == 0;
    int arity = is_new ? 0 : is_append ? 2 : 1;
    for (int ai = 0; ai < node->as.call.arg_count; ai++) {
        if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
    }
    if (node->as.call.arg_count != arity) {
        char msg[128];
        if (arity == 0) {
            snprintf(msg, sizeof msg, "%s expects no arguments", nbuf);
        } else {
            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                     nbuf, arity, arity == 1 ? "" : "s");
        }
        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
        node->semantic_type = ty(ctx, (is_new || is_with_alloc) ? TYPE_STRBUF : is_to_str ? TYPE_STR : TYPE_INT);
        return;
    }
    if (is_new) {
        node->semantic_type = ty(ctx, TYPE_STRBUF);
        return;
    }
    if (is_with_alloc) {
        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), at, "str_buf_with_allocator argument");
        node->semantic_type = ty(ctx, TYPE_STRBUF);
        return;
    }
    SemanticType *bt = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
    {
        char where[32];
        snprintf(where, sizeof where, "%s argument", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_STRBUF), bt, where);
    }
    if (is_append) {
        SemanticType *st = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_STR), st, "str_buf_append value");
    }
    node->semantic_type = ty(ctx, is_to_str ? TYPE_STR : TYPE_INT);
}

// M13.1-P5: Per-builtin checker for the Map builtins (LANGUAGE_SPEC
// §19.9). map_set's (map, str, int) signature does not fit the generic
// Builtin table in AST_CALL, so all are handled explicitly here,
// cloned from check_strbuf_builtin. TYPE_MAP is not parametrized — keys
// are always str and values always int.
void check_map_builtin(SemanticContext *ctx, AstNode *node, Token name) {
    char nbuf[32];
    int nlen = (int)name.length < 31 ? (int)name.length : 31;
    memcpy(nbuf, name.start, (size_t)nlen);
    nbuf[nlen] = '\0';
    bool is_new = strcmp(nbuf, "map_new") == 0;
    bool is_with_alloc = strcmp(nbuf, "map_with_allocator") == 0;
    bool is_set = strcmp(nbuf, "map_set") == 0;
    bool is_get = strcmp(nbuf, "map_get") == 0;
    bool is_has = strcmp(nbuf, "map_has") == 0;
    bool is_key_at = strcmp(nbuf, "map_key_at") == 0;
    bool is_val_at = strcmp(nbuf, "map_val_at") == 0;
    int arity = is_new ? 0 : is_set ? 3 : (is_get || is_has || is_key_at || is_val_at) ? 2 : 1;
    for (int ai = 0; ai < node->as.call.arg_count; ai++) {
        if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
    }
    PrimitiveType ret = (is_new || is_with_alloc) ? TYPE_MAP : is_has ? TYPE_BOOL : is_key_at ? TYPE_STR : TYPE_INT;
    if (node->as.call.arg_count != arity) {
        char msg[128];
        if (arity == 0) {
            snprintf(msg, sizeof msg, "%s expects no arguments", nbuf);
        } else {
            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                     nbuf, arity, arity == 1 ? "" : "s");
        }
        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
        node->semantic_type = ty(ctx, ret);
        return;
    }
    if (is_new) {
        node->semantic_type = ty(ctx, TYPE_MAP);
        return;
    }
    if (is_with_alloc) {
        SemanticType *at = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), at, "map_with_allocator argument");
        node->semantic_type = ty(ctx, TYPE_MAP);
        return;
    }
    SemanticType *mt = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
    {
        char where[32];
        snprintf(where, sizeof where, "%s argument", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_MAP), mt, where);
    }
    if (is_set || is_get || is_has) {
        SemanticType *kt = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        char where[32];
        snprintf(where, sizeof where, "%s key", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_STR), kt, where);
    }
    if (is_set) {
        SemanticType *vt = node->as.call.args[2] ? node->as.call.args[2]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), vt, "map_set value");
    }
    if (is_key_at || is_val_at) {
        SemanticType *it = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        char where[32];
        snprintf(where, sizeof where, "%s index", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), it, where);
    }
    node->semantic_type = ty(ctx, ret);
}

