// Match pattern checking for the semantic checker.
// Split from the monolithic src/semantic.c.
#include "../include/semantic_int.h"
#include <stdio.h>
#include <string.h>


// Pre-M13 S1: enum variant patterns require an integer scrutinee
// (enums emit i64 constants; matching them against a non-integer
// scrutinee can never succeed and is rejected before code generation).
static bool is_integer_scrutinee_kind(SemanticType *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_INT:
        case TYPE_I8: case TYPE_I16: case TYPE_I32:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
            return true;
        default:
            return false;
    }
}

// Validate a match-arm pattern against the scrutinee type.  Bindings are
// defined in `arm_env` (the arm-local scope).  Recursive for constructor
// sub-patterns.
void check_pattern(SemanticContext *ctx, Pattern *pat, SemanticType *scrutinee_type, Environment *arm_env) {
    if (!pat) return;
    switch (pat->kind) {
        case PAT_WILDCARD:
            // Always matches, no bindings.
            break;
        case PAT_LITERAL: {
            // Type-check the literal expression node, then unify with scrutinee.
            check_node(ctx, pat->as.literal.expr);
            SemanticType *lit_type = pat->as.literal.expr ?
                pat->as.literal.expr->semantic_type : NULL;
            if (lit_type && scrutinee_type &&
                lit_type->kind != TYPE_UNKNOWN && scrutinee_type->kind != TYPE_UNKNOWN) {
                unify(ctx, pat->token.line, scrutinee_type, lit_type,
                      "pattern type mismatch");
            }
            pat->semantic_type = scrutinee_type;
            break;
        }
        case PAT_BINDING: {
            // Define an immutable binding in the arm scope.
            // env_define returns false if the name already exists in this scope
            // (duplicate binding within the same pattern).
            if (!env_define(arm_env, pat->as.binding.name, false, scrutinee_type)) {
                char msg[160];
                snprintf(msg, sizeof msg, "duplicate binding '%.*s' in pattern",
                         (int)pat->as.binding.name.length, pat->as.binding.name.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_IMMUTABLE_ASSIGNMENT, msg);
            }
            pat->semantic_type = scrutinee_type;
            break;
        }
        case PAT_CONSTRUCTOR: {
            Token cname = pat->as.constructor.name;
            bool is_some = cname.length == 4 && memcmp(cname.start, "some", 4) == 0;
            bool is_ok   = cname.length == 2 && memcmp(cname.start, "ok", 2) == 0;
            bool is_err  = cname.length == 3 && memcmp(cname.start, "err", 3) == 0;

            if (!is_some && !is_ok && !is_err) {
                char msg[160];
                snprintf(msg, sizeof msg, "unknown constructor '%.*s'",
                         (int)cname.length, cname.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_UNDEFINED_SYMBOL, msg);
                break;
            }

            if (is_some) {
                if (!scrutinee_type || scrutinee_type->kind != TYPE_OPTION) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_TYPE_MISMATCH,
                               "'some' pattern requires Option scrutinee");
                    break;
                }
                if (pat->as.constructor.arg_count != 1) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_ARITY_MISMATCH,
                               "'some' pattern requires exactly 1 argument");
                    break;
                }
                SemanticType *inner = scrutinee_type->inner_type;
                if (!inner) inner = ty(ctx, TYPE_UNKNOWN);
                check_pattern(ctx, pat->as.constructor.args[0], inner, arm_env);
            } else if (is_ok) {
                if (!scrutinee_type || scrutinee_type->kind != TYPE_RESULT) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_TYPE_MISMATCH,
                               "'ok' pattern requires Result scrutinee");
                    break;
                }
                if (pat->as.constructor.arg_count != 1) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_ARITY_MISMATCH,
                               "'ok' pattern requires exactly 1 argument");
                    break;
                }
                SemanticType *val_type = scrutinee_type->inner_type;
                if (!val_type) val_type = ty(ctx, TYPE_UNKNOWN);
                check_pattern(ctx, pat->as.constructor.args[0], val_type, arm_env);
            } else { // is_err
                if (!scrutinee_type || scrutinee_type->kind != TYPE_RESULT) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_TYPE_MISMATCH,
                               "'err' pattern requires Result scrutinee");
                    break;
                }
                if (pat->as.constructor.arg_count != 1) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_ARITY_MISMATCH,
                               "'err' pattern requires exactly 1 argument");
                    break;
                }
                SemanticType *err_type = scrutinee_type->error_type;
                if (!err_type) err_type = ty(ctx, TYPE_UNKNOWN);
                check_pattern(ctx, pat->as.constructor.args[0], err_type, arm_env);
            }
            break;
        }
        case PAT_ENUM_VARIANT: {
            EnumEntry *ee = enum_lookup(ctx, pat->as.enum_variant.type_name);
            if (!ee) {
                char msg[160];
                snprintf(msg, sizeof msg, "unknown enum '%.*s'",
                         (int)pat->as.enum_variant.type_name.length,
                         pat->as.enum_variant.type_name.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_UNDEFINED_SYMBOL, msg);
                break;
            }
            if (enum_variant_index(ee, pat->as.enum_variant.variant_name) < 0) {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "unknown variant '%.*s' of enum '%.*s'",
                         (int)pat->as.enum_variant.variant_name.length,
                         pat->as.enum_variant.variant_name.start,
                         (int)pat->as.enum_variant.type_name.length,
                         pat->as.enum_variant.type_name.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_UNKNOWN_VARIANT, msg);
            }
            // Pre-M13 S1: enum variants are i64 constants, so the
            // scrutinee must be an integer kind — matching against str
            // (or any non-integer type) is unreachable and rejected.
            if (scrutinee_type && scrutinee_type->kind != TYPE_UNKNOWN &&
                !is_integer_scrutinee_kind(scrutinee_type)) {
                char tbuf[64];
                type_display(scrutinee_type, tbuf, sizeof tbuf);
                char msg[200];
                snprintf(msg, sizeof msg,
                         "enum variant pattern requires integer scrutinee, found %s",
                         tbuf);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_TYPE_MISMATCH, msg);
            }
            break;
        }
    }
}
