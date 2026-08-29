// Type unification and compatibility helpers for the semantic checker.
// Split from the monolithic src/semantic.c.
#include "../include/semantic_int.h"
#include <stdio.h>

bool optres_inner_same(SemanticType *a, SemanticType *b) {
    if (!a || !b) return true;
    if (a->kind == TYPE_UNKNOWN || b->kind == TYPE_UNKNOWN) return true;
    if (a->kind != b->kind) return false;
    if (a->kind == TYPE_STRUCT) return a == b;
    if (a->kind == TYPE_OPTION)
        return optres_inner_same(a->inner_type, b->inner_type);
    if (a->kind == TYPE_RESULT)
        return optres_inner_same(a->inner_type, b->inner_type) &&
               optres_inner_same(a->error_type, b->error_type);
    return true;
}

// The single kind-level compatibility rule (OPTIMIZATION_PLAN 3.1).
// Unknown unifies with anything and takes the known side; otherwise the
// kinds must match exactly. For Option/Result types, inner (and error)
// types must also unify so that e.g. Result<Option<i64>,i64> and
// Result<Option<Option<i64>>,i64> are not silently treated as
// interchangeable (Pre-M13 S4).
// On mismatch this emits
// "<context>: expected <T>, found <U>" and returns NULL.
SemanticType *unify(SemanticContext *ctx, int line,
                           SemanticType *expected, SemanticType *found,
                           const char *context) {
    if (!expected) return found;
    if (!found) return expected;
    if (expected->kind == TYPE_UNKNOWN) return found;
    if (found->kind == TYPE_UNKNOWN) return expected;
    if (expected->kind != found->kind) {
        char want[96];
        char got[96];
        char msg[320];
        type_display(expected, want, sizeof want);
        type_display(found, got, sizeof got);
        snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
        diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
        return NULL;
    }
    // Same kind. For Option/Result, check inner/error match structurally.
    if (expected->kind == TYPE_OPTION) {
        if (!optres_inner_same(expected->inner_type, found->inner_type)) {
            char want[96], got[96], msg[320];
            type_display(expected, want, sizeof want);
            type_display(found, got, sizeof got);
            snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
            diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
            return NULL;
        }
        return expected;
    }
    if (expected->kind == TYPE_RESULT) {
        if (!optres_inner_same(expected->inner_type, found->inner_type) ||
            !optres_inner_same(expected->error_type, found->error_type)) {
            char want[96], got[96], msg[320];
            type_display(expected, want, sizeof want);
            type_display(found, got, sizeof got);
            snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
            diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
            return NULL;
        }
        return expected;
    }
    return expected;
}

bool eq_comparable_kind(PrimitiveType k) {
    return k == TYPE_INT || k == TYPE_FLOAT || k == TYPE_BOOL ||
           k == TYPE_I8  || k == TYPE_I16 || k == TYPE_I32 ||
           k == TYPE_U8  || k == TYPE_U16 || k == TYPE_U32 || k == TYPE_U64 ||
           k == TYPE_F32;
}

bool optres_payload_ok(SemanticType *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_INT:
        case TYPE_I8: case TYPE_I16: case TYPE_I32:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
        case TYPE_F32: case TYPE_FLOAT: case TYPE_BOOL:
        case TYPE_STRUCT:
            return true;
        case TYPE_OPTION:
            return optres_payload_ok(t->inner_type);
        case TYPE_RESULT:
            return optres_payload_ok(t->inner_type) &&
                   optres_payload_ok(t->error_type);
        default:
            return false;
    }
}

void optres_payload_reject(SemanticContext *ctx, int line, bool is_result,
                           SemanticType *t) {
    char tbuf[96];
    char msg[224];
    type_display(t, tbuf, sizeof tbuf);
    snprintf(msg, sizeof msg, "%s payload type '%s' is not supported (S4 boundary)",
             is_result ? "Result" : "Option", tbuf);
    diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
}

bool optres_type_is_flat(SemanticType *t) {
    if (!t) return true;
    if (t->kind == TYPE_OPTION)
        return !t->inner_type ||
               (t->inner_type->kind == TYPE_INT && !t->inner_type->element_type &&
                t->inner_type->array_length == 0 && t->inner_type->param_count == 0);
    if (t->kind == TYPE_RESULT) {
        bool inner_flat = !t->inner_type ||
                          (t->inner_type->kind == TYPE_INT && !t->inner_type->element_type &&
                           t->inner_type->array_length == 0 && t->inner_type->param_count == 0);
        bool error_flat = !t->error_type ||
                          (t->error_type->kind == TYPE_INT && !t->error_type->element_type &&
                           t->error_type->array_length == 0 && t->error_type->param_count == 0);
        return inner_flat && error_flat;
    }
    return true;
}

// M16.2: FFI-safe kinds per the C ABI mapping table (LANGUAGE_SPEC §7.1).
// Everything else fails closed at the extern declaration site (E29).
bool ffi_safe_kind(PrimitiveType k) {
    return k == TYPE_I8 || k == TYPE_I16 || k == TYPE_I32 || k == TYPE_INT ||
           k == TYPE_U8 || k == TYPE_U16 || k == TYPE_U32 || k == TYPE_U64 ||
           k == TYPE_F32 || k == TYPE_FLOAT || k == TYPE_BOOL || k == TYPE_STR ||
           k == TYPE_STRUCT;
}

