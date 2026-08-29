// Type unification and compatibility helpers for the semantic checker.
// Split from the monolithic src/semantic.c.
#include "../include/semantic_int.h"
#include <stdio.h>

// Pre-M13 S4: structural comparison for Option/Result inner (and error)
// types. NULL and TYPE_UNKNOWN sides are tolerated (inference may not
// have resolved them yet). Scalars compare by kind: per-arity function
// returns intern distinct scalar instances (type_get_func), so pooled
// pointer identity would falsely reject valid programs. Structs are
// nominal (declaration-site identity). Nested Option/Result recurse.
static bool optres_inner_same(SemanticType *a, SemanticType *b) {
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

// Pre-M13 S4: types whose values support == / != without inheriting
// ambiguous C behaviour.  Numeric kinds, bool, and enum variants
// (represented as i64) compare by value; everything else — str (pointer
// vs content), struct (no C ==), array/slice/vec/strbuf/map (handles or
// address decay), option/result (opaque int64_t slots), stream, chan,
// ref — must fail closed (LANGUAGE_SPEC §17.1 equality surface).
bool eq_comparable_kind(PrimitiveType k) {
    return k == TYPE_INT || k == TYPE_FLOAT || k == TYPE_BOOL ||
           k == TYPE_I8  || k == TYPE_I16 || k == TYPE_I32 ||
           k == TYPE_U8  || k == TYPE_U16 || k == TYPE_U32 || k == TYPE_U64 ||
           k == TYPE_F32;
}

// M16.2: FFI-safe kinds per the C ABI mapping table (LANGUAGE_SPEC §7.1).
// Everything else fails closed at the extern declaration site (E29).
bool ffi_safe_kind(PrimitiveType k) {
    return k == TYPE_I8 || k == TYPE_I16 || k == TYPE_I32 || k == TYPE_INT ||
           k == TYPE_U8 || k == TYPE_U16 || k == TYPE_U32 || k == TYPE_U64 ||
           k == TYPE_F32 || k == TYPE_FLOAT || k == TYPE_BOOL || k == TYPE_STR ||
           k == TYPE_STRUCT;
}

