// C11 emission backend for Tiq (split out of main.c per plan 2.1).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/lexer.h"
#include "../include/diag.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/type.h"
#include "../include/emit_c.h"
#include "../include/runtime_prelude.h"

// Emit the raw bytes of a Tiq string literal as a C string literal.
// LANGUAGE_SPEC §4 escapes (\\ \" \n \r \t \0) are spelled identically in C,
// so validated escape sequences pass through verbatim; the lexer has already
// rejected any other escape. Bare control bytes are hex-escaped.
static void emit_c_string(FILE *out, const char *start, size_t length) {
    size_t i;
    fputc('"', out);
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)start[i];
        if (ch == '\\' && i + 1 < length) {
            fputc('\\', out);
            fputc(start[++i], out);
        } else if (ch < 32U || ch == 127U) {
            fprintf(out, "\\x%02x", ch);
        } else {
            fputc((int)ch, out);
        }
    }
    fputc('"', out);
}

#define TIQ_MAX_STREAM_GENS 64
typedef struct { const char *name; Token *params; int param_count; } EmitStreamGenInfo;

// All emitter state lives here; no file-static mutable globals, so the
// backend is re-entrant and unit-testable (plan 2.1).
typedef struct EmitContext {
    FILE *out;
    DiagContext *diag;
    const char *path;
    EmitStreamGenInfo stream_gens[TIQ_MAX_STREAM_GENS];
    int stream_gen_count;
    // M9.1: enclosing function during body emission; used to re-derive
    // reference parameters (semantic analysis auto-derefs their uses).
    AstNode *current_fn;
} EmitContext;

static void emit_expr(AstNode *node, EmitContext *ctx);
static void emit_stmt(AstNode *node, EmitContext *ctx, int indent);
static void emit_type_name(PrimitiveType kind, FILE *out);

// M9.1: 0 = not a reference parameter of the enclosing function,
// 1 = shared borrow (&T), 2 = mutable borrow (&mut T).
static int ref_param_kind(EmitContext *ctx, Token name) {
    AstNode *fn = ctx->current_fn;
    if (!fn) return 0;
    for (int i = 0; i < fn->as.function.param_count; i++) {
        if (fn->as.function.params[i].length == name.length &&
            memcmp(fn->as.function.params[i].start, name.start, name.length) == 0) {
            SemanticType *pt = fn->as.function.param_types ?
                (SemanticType *)fn->as.function.param_types[i] : NULL;
            if (pt && pt->kind == TYPE_REF) return 1;
            if (pt && pt->kind == TYPE_REF_MUT) return 2;
            return 0;
        }
    }
    return 0;
}

static bool is_stream_gen_name(EmitContext *ctx, const char *name, int len, int *out_params, int *out_param_count) {
    for (int i = 0; i < ctx->stream_gen_count; i++) {
        if ((int)strlen(ctx->stream_gens[i].name) == len &&
            memcmp(ctx->stream_gens[i].name, name, len) == 0) {
            *out_params = 0;
            *out_param_count = ctx->stream_gens[i].param_count;
            return true;
        }
    }
    return false;
}

static const char *binary_op_c_str(TokenKind op) {
    switch (op) {
        case TOK_PLUS: return "+"; case TOK_MINUS: return "-"; case TOK_STAR: return "*";
        case TOK_SLASH: return "/"; case TOK_PERCENT: return "%";
        case TOK_EQ_EQ: return "=="; case TOK_BANG_EQ: return "!=";
        case TOK_LT: return "<"; case TOK_LTE: return "<="; case TOK_GT: return ">"; case TOK_GTE: return ">=";
        case TOK_AND_AND: return "&&"; case TOK_OR_OR: return "||";
        case TOK_AMP: return "&"; case TOK_PIPE: return "|"; case TOK_CARET: return "^";
        case TOK_LSHIFT: return "<<"; case TOK_RSHIFT: return ">>";
        default: return "?";
    }
}

static void emit_expr(AstNode *node, EmitContext *ctx) {
    if (!node) return;
    switch (node->kind) {
        case AST_LITERAL: {
            if (node->as.literal.type == TOK_INT)
                // Integer literals are i64 (LANGUAGE_SPEC §11); suffix keeps
                // C constant arithmetic in 64-bit before any conversion.
                fprintf(ctx->out, "%.*sLL", (int)node->token.length, node->token.start);
            else if (node->as.literal.type == TOK_FLOAT)
                fprintf(ctx->out, "%.*s", (int)node->token.length, node->token.start);
            else if (node->as.literal.type == TOK_STRING) emit_c_string(ctx->out, node->token.start + 1, node->token.length - 2);
            else if (node->as.literal.type == TOK_TRUE) fputs("1", ctx->out);
            else if (node->as.literal.type == TOK_FALSE) fputs("0", ctx->out);
            break;
        }
        case AST_IDENTIFIER:
            // M8: 'none' is a polymorphic Option constructor keyword.
            if (node->as.identifier.name.length == 4 &&
                memcmp(node->as.identifier.name.start, "none", 4) == 0) {
                fputs("((TiqOption){ .value = 0, .has_value = 0 })", ctx->out);
            } else if (ref_param_kind(ctx, node->as.identifier.name) != 0) {
                // M9.1: reference parameters deref to the referent.
                fprintf(ctx->out, "(*%.*s)", (int)node->as.identifier.name.length, node->as.identifier.name.start);
            } else {
                fprintf(ctx->out, "%.*s", (int)node->as.identifier.name.length, node->as.identifier.name.start);
            }
            break;
        case AST_BINARY: {
            // M8: Fallback operator ?? for Option/Result types.
            if (node->as.binary.op == TOK_QUESTION_QUESTION) {
                SemanticType *lt = node->as.binary.left ? node->as.binary.left->semantic_type : NULL;
                const char *flag_field = (lt && lt->kind == TYPE_RESULT) ? ".is_ok" : ".has_value";
                fputs("(", ctx->out);
                emit_expr(node->as.binary.left, ctx);
                fputs(flag_field, ctx->out);
                fputs(" ? ", ctx->out);
                emit_expr(node->as.binary.left, ctx);
                fputs(".value : ", ctx->out);
                emit_expr(node->as.binary.right, ctx);
                fputs(")", ctx->out);
                break;
            }
            fputs("(", ctx->out);
            emit_expr(node->as.binary.left, ctx);
            fputs(" ", ctx->out);
            fputs(binary_op_c_str(node->as.binary.op), ctx->out);
            fputs(" ", ctx->out);
            emit_expr(node->as.binary.right, ctx);
            fputs(")", ctx->out);
            break;
        }
        case AST_UNARY: {
            if (node->as.unary.op == TOK_MOVE) {
                emit_expr(node->as.unary.right, ctx);
            } else if (node->as.unary.op == TOK_QUESTION) {
                // M8: Propagation operator - unwrap Option/Result value.
                // Full early-return semantics not yet implemented; emit .value access.
                emit_expr(node->as.unary.right, ctx);
                fputs(".value", ctx->out);
            } else if (node->as.unary.op == TOK_AMP) {
                // M9.1: borrow argument; semantic analysis guarantees the
                // operand is a plain named binding in the caller's scope.
                fputs("&", ctx->out);
                emit_expr(node->as.unary.right, ctx);
            } else {
                const char *op = "";
                if (node->as.unary.op == TOK_BANG) op = "!";
                else if (node->as.unary.op == TOK_MINUS) op = "-";
                else if (node->as.unary.op == TOK_PLUS) op = "+";
                fputs(op, ctx->out); fputs("(", ctx->out);
                emit_expr(node->as.unary.right, ctx);
                fputs(")", ctx->out);
            }
            break;
        }
        case AST_CONDITIONAL:
            emit_expr(node->as.conditional.cond, ctx);
            fputs(" ? ", ctx->out);
            emit_expr(node->as.conditional.then_branch, ctx);
            fputs(" : ", ctx->out);
            emit_expr(node->as.conditional.else_branch, ctx);
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 5 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "print", 5) == 0 &&
                node->as.call.arg_count == 1) {
                // print builtin: one printf call per printable type; the
                // expression value is printf's return (bytes written).
                AstNode *arg = node->as.call.args[0];
                SemanticType *st = arg ? arg->semantic_type : NULL;
                PrimitiveType kind = st ? st->kind : TYPE_INT;
                if (kind == TYPE_STR) {
                    fputs("printf(\"%s\\n\", ", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(")", ctx->out);
                } else if (kind == TYPE_FLOAT || kind == TYPE_F32) {
                    fputs("printf(\"%g\\n\", (double)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs("))", ctx->out);
                } else if (kind == TYPE_BOOL) {
                    fputs("printf(\"%s\\n\", (", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(") ? \"true\" : \"false\")", ctx->out);
                } else if (kind == TYPE_STR_VIEW || kind == TYPE_SLICE) {
                    fputs("printf(\"%.*s\\n\", ((TiqSlice)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(")).len, (const char*)(((TiqSlice)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(")).ptr))", ctx->out);
                } else {
                    fputs("printf(\"%lld\\n\", (long long)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs("))", ctx->out);
                }
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 3 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "len", 3) == 0) {
                SemanticType *ct = node->as.call.args[0] ?
                    node->as.call.args[0]->semantic_type : NULL;
                if (ct && ct->kind == TYPE_ARRAY) {
                    fprintf(ctx->out, "%d", ct->array_length);
                } else if (ct && (ct->kind == TYPE_SLICE || ct->kind == TYPE_STR_VIEW)) {
                    fputs("(((TiqSlice)", ctx->out);
                    emit_expr(node->as.call.args[0], ctx);
                    fputs(").len)", ctx->out);
                } else if (ct && ct->kind == TYPE_STR) {
                    fputs("((int)strlen(", ctx->out);
                    emit_expr(node->as.call.args[0], ctx);
                    fputs("))", ctx->out);
                } else {
                    fprintf(ctx->out, "0");
                }
                break;
            }
            // M8: Option constructors some(x) and none.
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 4 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "some", 4) == 0 &&
                node->as.call.arg_count == 1) {
                fputs("((TiqOption){ .value = (int64_t)(", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs("), .has_value = 1 })", ctx->out);
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 4 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "none", 4) == 0 &&
                node->as.call.arg_count == 0) {
                fputs("((TiqOption){ .value = 0, .has_value = 0 })", ctx->out);
                break;
            }
            // M8: Result constructors ok(x) and err(e).
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 2 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "ok", 2) == 0 &&
                node->as.call.arg_count == 1) {
                fputs("((TiqResult){ .value = (int64_t)(", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs("), .error = 0, .is_ok = 1 })", ctx->out);
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 3 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "err", 3) == 0 &&
                node->as.call.arg_count == 1) {
                fputs("((TiqResult){ .value = 0, .error = (int64_t)(", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs("), .is_ok = 0 })", ctx->out);
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                typedef struct { const char *tiq; int len; const char *c; } Btn;
                static const Btn btn[] = {
                    {"fs_read", 7, "tiq_fs_read"}, {"fs_write", 8, "tiq_fs_write"},
                    {"fs_exists", 9, "tiq_fs_exists"}, {"proc_exec", 9, "tiq_proc_exec"},
                    {"proc_exit", 9, "tiq_proc_exit"}, {"json_parse_int", 14, "tiq_json_parse_int"},
                    {"json_encode_str", 15, "tiq_json_encode_str"}, {"net_fetch", 9, "tiq_net_fetch"},
                    {"cli_arg_count", 13, "tiq_cli_arg_count"}, {"cli_arg", 7, "tiq_cli_arg"},
                    {"json_get", 8, "tiq_json_get"},
                };
                const char *builtin_fn = NULL;
                for (int bi = 0; bi < (int)(sizeof btn / sizeof btn[0]); bi++) {
                    if ((int)name.length == btn[bi].len && memcmp(name.start, btn[bi].tiq, name.length) == 0) {
                        builtin_fn = btn[bi].c;
                        break;
                    }
                }
                if (builtin_fn) {
                    fprintf(ctx->out, "%s(", builtin_fn);
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        if (i > 0) fputs(", ", ctx->out);
                        if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                    }
                    fputs(")", ctx->out);
                    break;
                }
                // M12.3: Explicit numeric type conversion.
                // The semantic checker already resolved node->semantic_type to the
                // target type. Detect by matching the callee name against the
                // conversion table and checking the result is a scalar primitive.
                // Emit: ((C_type)(arg_expr))
                {
                    typedef struct { const char *n; int len; } CnvName;
                    static const CnvName cnv[] = {
                        {"i8",2},{"i16",3},{"i32",3},{"i64",3},
                        {"u8",2},{"u16",3},{"u32",3},{"u64",3},
                        {"f32",3},{"f64",3},{"bool",4},{"str",3},
                    };
                    int is_conv = 0;
                    for (int k = 0; k < (int)(sizeof cnv/sizeof cnv[0]); k++) {
                        if ((int)name.length == cnv[k].len &&
                            memcmp(name.start, cnv[k].n, (size_t)cnv[k].len) == 0) {
                            is_conv = 1; break;
                        }
                    }
                    SemanticType *rst = node->semantic_type;
                    // Only emit the cast if the result is a scalar numeric/bool type
                    // (not array/slice/unknown/struct). Unknown means an error was
                    // already reported; fall through to avoid double output.
                    bool result_scalar = rst && (
                        rst->kind == TYPE_INT   || rst->kind == TYPE_FLOAT ||
                        rst->kind == TYPE_BOOL  || rst->kind == TYPE_I8    ||
                        rst->kind == TYPE_I16   || rst->kind == TYPE_I32   ||
                        rst->kind == TYPE_U8    || rst->kind == TYPE_U16   ||
                        rst->kind == TYPE_U32   || rst->kind == TYPE_U64   ||
                        rst->kind == TYPE_F32);
                    if (is_conv && result_scalar && node->as.call.arg_count == 1 &&
                        node->as.call.args[0]) {
                        fputs("((", ctx->out);
                        emit_type_name(rst->kind, ctx->out);
                        fputs(")(", ctx->out);
                        emit_expr(node->as.call.args[0], ctx);
                        fputs("))", ctx->out);
                        break;
                    }
                }
            }

            if (node->as.call.is_bracket_call && node->as.call.is_slice && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                fputs("((TiqSlice){ .ptr = ", ctx->out);
                if (ct && (ct->kind == TYPE_SLICE || ct->kind == TYPE_STR_VIEW)) {
                    fputs("((const char*)(((TiqSlice)", ctx->out);
                    emit_expr(node->as.call.callee, ctx);
                    fputs(").ptr) + (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") * ", ctx->out);
                    if (ct->kind == TYPE_SLICE) fputs("sizeof(int64_t)", ctx->out);
                    else fputs("1", ctx->out);
                    fputs("), .len = (", ctx->out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], ctx);
                    else {
                        fputs("(((TiqSlice)", ctx->out);
                        emit_expr(node->as.call.callee, ctx);
                        fputs(").len)", ctx->out);
                    }
                    fputs(") - (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") })", ctx->out);
                } else if (ct && ct->kind == TYPE_STR) {
                    fputs("((const char*)(", ctx->out);
                    emit_expr(node->as.call.callee, ctx);
                    fputs(") + (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(")), .len = (", ctx->out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], ctx);
                    else {
                        fputs("((int)strlen(", ctx->out);
                        emit_expr(node->as.call.callee, ctx);
                        fputs("))", ctx->out);
                    }
                    fputs(") - (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") })", ctx->out);
                } else {
                    int arr_len = ct ? ct->array_length : 0;
                    fputs("((const char*)(", ctx->out);
                    emit_expr(node->as.call.callee, ctx);
                    fputs(") + (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") * sizeof(int64_t)), .len = (", ctx->out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], ctx);
                    else fprintf(ctx->out, "%d", arr_len);
                    fputs(") - (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") })", ctx->out);
                }
                break;
            }
            if (node->as.call.is_bracket_call && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                if (ct && (ct->kind == TYPE_ARRAY || ct->kind == TYPE_SLICE)) {
                    if (ct->kind == TYPE_ARRAY) {
                        int len = ct->array_length;
                        if (len > 0 && node->as.call.arg_count > 0 && node->as.call.args[0]) {
                            fputs("((uint64_t)(", ctx->out);
                            emit_expr(node->as.call.args[0], ctx);
                            fprintf(ctx->out, ") < (uint64_t)(%d) ? ", len);
                            emit_expr(node->as.call.callee, ctx);
                            fputs("[", ctx->out);
                            for (int i = 0; i < node->as.call.arg_count; i++) {
                                if (i > 0) fputs("][", ctx->out);
                                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                            }
                            fputs("] : (fprintf(stderr, \"tiq: index %lld out of bounds for array of length %d\\n\", (long long)(", ctx->out);
                            emit_expr(node->as.call.args[0], ctx);
                            fprintf(ctx->out, "), %d), exit(1), ", len);
                            emit_expr(node->as.call.callee, ctx);
                            fputs("[", ctx->out);
                            for (int i = 0; i < node->as.call.arg_count; i++) {
                                if (i > 0) fputs("][", ctx->out);
                                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                            }
                            fputs("]))", ctx->out);
                        } else {
                            emit_expr(node->as.call.callee, ctx);
                            fputs("[", ctx->out);
                            for (int i = 0; i < node->as.call.arg_count; i++) {
                                if (i > 0) fputs("][", ctx->out);
                                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                            }
                            fputs("]", ctx->out);
                        }
                    } else {
                        fputs("((const int64_t*)(((TiqSlice)(", ctx->out);
                        emit_expr(node->as.call.callee, ctx);
                        fputs(")).ptr))[", ctx->out);
                        if (node->as.call.arg_count > 0 && node->as.call.args[0])
                            emit_expr(node->as.call.args[0], ctx);
                        else
                            fputs("0", ctx->out);
                        fputs("]", ctx->out);
                    }
                    break;
                }
                if (node->as.call.callee->kind == AST_IDENTIFIER) {
                    fprintf(ctx->out, "tiq_gen_%.*s(", (int)node->as.call.callee->as.identifier.name.length, node->as.call.callee->as.identifier.name.start);
                } else if (node->as.call.callee->kind == AST_CALL &&
                           node->as.call.callee->as.call.callee &&
                           node->as.call.callee->as.call.callee->kind == AST_IDENTIFIER) {
                    AstNode *inner_fn = node->as.call.callee->as.call.callee;
                    int dummy1, fn_param_count;
                    if (is_stream_gen_name(ctx, inner_fn->as.identifier.name.start,
                                           (int)inner_fn->as.identifier.name.length,
                                           &dummy1, &fn_param_count)) {
                        fprintf(ctx->out, "tiq_gen_%.*s(", (int)inner_fn->as.identifier.name.length, inner_fn->as.identifier.name.start);
                        for (int ai = 0; ai < node->as.call.callee->as.call.arg_count; ai++) {
                            if (ai > 0) fputs(", ", ctx->out);
                            if (node->as.call.callee->as.call.args[ai])
                                emit_expr(node->as.call.callee->as.call.args[ai], ctx);
                        }
                        if (node->as.call.arg_count > 0) fputs(", ", ctx->out);
                    } else {
                        emit_expr(node->as.call.callee, ctx);
                        fputs("(", ctx->out);
                    }
                } else {
                    emit_expr(node->as.call.callee, ctx);
                    fputs("(", ctx->out);
                }
            } else {
                emit_expr(node->as.call.callee, ctx);
                fputs("(", ctx->out);
            }
            for (int i = 0; i < node->as.call.arg_count; i++) {
                if (i > 0) fputs(", ", ctx->out);
                SemanticType *arg_t = node->as.call.args[i] ? node->as.call.args[i]->semantic_type : NULL;
                if (arg_t && arg_t->kind == TYPE_ARRAY && !node->as.call.is_bracket_call) {
                    fputs("(TiqSlice){ .ptr = ", ctx->out);
                    emit_expr(node->as.call.args[i], ctx);
                    fprintf(ctx->out, ", .len = %d }", arg_t->array_length);
                } else {
                    if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                }
            }
            fputs(")", ctx->out);
            break;
        case AST_ARRAY: {
            fputs("{", ctx->out);
            for (int i = 0; i < node->as.array.element_count; i++) {
                if (i > 0) fputs(", ", ctx->out);
                emit_expr(node->as.array.elements[i], ctx);
            }
            fputs("}", ctx->out);
            break;
        }
        case AST_ARRAY_FILL: {
            // For array fill [value; length], we need to emit all elements explicitly
            // because C initializers only zero-initialize the rest.
            // Check if we can determine the length at compile time.
            int len = 0;
            SemanticType *t = node->semantic_type;
            if (t && t->kind == TYPE_ARRAY) {
                len = t->array_length;
            }
            if (len > 0) {
                // We know the length: emit explicit element list
                fputs("{ ", ctx->out);
                emit_expr(node->as.array_fill.value, ctx);
                for (int i = 1; i < len; i++) {
                    fputs(", ", ctx->out);
                    emit_expr(node->as.array_fill.value, ctx);
                }
                fputs(" }", ctx->out);
            } else {
                // Unknown length: emit just the first element (defensive)
                // This case shouldn't normally occur with proper type inference
                fputs("{ ", ctx->out);
                emit_expr(node->as.array_fill.value, ctx);
                fputs(" }", ctx->out);
            }
            break;
        }
        case AST_FIELD_ACCESS:
            emit_expr(node->as.field_access.target, ctx);
            fputs(".", ctx->out);
            fprintf(ctx->out, "%.*s", (int)node->as.field_access.field.length, node->as.field_access.field.start);
            break;
        case AST_RECORD_LIT: {
            // M12.6: Emit C struct initializer
            SemanticType *st = node->semantic_type;
            if (st && st->kind == TYPE_STRUCT && st->struct_name) {
                fprintf(ctx->out, "(%s){ ", st->struct_name);
                for (int i = 0; i < node->as.record_lit.field_count; i++) {
                    if (i > 0) fputs(", ", ctx->out);
                    fprintf(ctx->out, ".%.*s = ", (int)node->as.record_lit.field_names[i].length,
                            node->as.record_lit.field_names[i].start);
                    emit_expr(node->as.record_lit.field_values[i], ctx);
                }
                fputs(" }", ctx->out);
            } else {
                fputs("0", ctx->out);
            }
            break;
        }
        case AST_SPAWN:
            // Unreachable after semantic rejection; fail closed if hit.
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "spawn is not supported yet");
            break;
        case AST_CHAN:
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "chan is not supported yet");
            break;
        case AST_MATCH: {
            // Emit: (cond1 ? body1 : (cond2 ? body2 : (... : 0)))
            // Wildcard arms match everything and should be the last arm
            // Structure: (cond) ? then : else  (parens close right after condition)
            bool has_wildcard = false;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                if (node->as.match_expr.arms[i].is_wildcard) {
                    emit_expr(node->as.match_expr.arms[i].body, ctx);
                    has_wildcard = true;
                } else {
                    fputs("(", ctx->out);
                    emit_expr(node->as.match_expr.expr, ctx);
                    fputs(" == ", ctx->out);
                    emit_expr(node->as.match_expr.arms[i].pattern, ctx);
                    fputs(") ? ", ctx->out);
                    emit_expr(node->as.match_expr.arms[i].body, ctx);
                    fputs(" : ", ctx->out);
                }
            }
            // Final fallback (only if no wildcard arm)
            if (!has_wildcard) {
                fputs("0", ctx->out);
            }
            break;
        }
        case AST_BLOCK:
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                       "block expression not supported outside function body");
            break;
        case AST_DEFER:
            if (node->as.defer.expr)
                emit_expr(node->as.defer.expr, ctx);
            break;
        default:
            break;
    }
}

static void emit_type_name(PrimitiveType kind, FILE *out) {
    switch (kind) {
        case TYPE_INT:      fputs("int64_t", out); break;
        case TYPE_FLOAT:    fputs("double", out); break;
        case TYPE_BOOL:     fputs("int64_t", out); break;
        case TYPE_STR:      fputs("const char *", out); break;
        case TYPE_ARRAY:    fputs("int64_t", out); break;
        case TYPE_SLICE:    fputs("TiqSlice", out); break;
        case TYPE_STR_VIEW: fputs("TiqSlice", out); break;
        case TYPE_STREAM:   fputs("int64_t", out); break;
        // Sized kinds map to stdint.h types (M12.2); no surface syntax
        // constructs them until explicit conversions land (M12.3).
        case TYPE_I8:       fputs("int8_t", out); break;
        case TYPE_I16:      fputs("int16_t", out); break;
        case TYPE_I32:      fputs("int32_t", out); break;
        case TYPE_U8:       fputs("uint8_t", out); break;
        case TYPE_U16:      fputs("uint16_t", out); break;
        case TYPE_U32:      fputs("uint32_t", out); break;
        case TYPE_U64:      fputs("uint64_t", out); break;
        case TYPE_F32:      fputs("float", out); break;
        case TYPE_UNIT:     fputs("void", out); break;
        case TYPE_NEVER:    fputs("void", out); break;
        case TYPE_OPTION:   fputs("TiqOption", out); break;
        case TYPE_RESULT:   fputs("TiqResult", out); break;
        default:           fputs("int64_t", out); break;
    }
}

// M12.6/M8: Emit full C type for a SemanticType, including struct names.
static void emit_semantic_type(SemanticType *t, FILE *out) {
    if (!t) { fputs("int64_t", out); return; }
    if (t->kind == TYPE_REF || t->kind == TYPE_REF_MUT) {
        // M9.1: &T -> const T *, &mut T -> T *.
        if (t->kind == TYPE_REF) fputs("const ", out);
        if (t->element_type) emit_type_name(t->element_type->kind, out);
        else fputs("int64_t", out);
        fputs(" *", out);
        return;
    }
    if (t->kind == TYPE_STRUCT && t->struct_name && t->struct_name[0]) {
        fputs(t->struct_name, out);
    } else {
        emit_type_name(t->kind, out);
    }
}

static void emit_stmt(AstNode *node, EmitContext *ctx, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) fputs("    ", ctx->out);
    switch (node->kind) {
        case AST_BINDING: {
            SemanticType *t = node->semantic_type;
            bool is_move = node->as.binding.expr && node->as.binding.expr->kind == AST_UNARY &&
                           node->as.binding.expr->as.unary.op == TOK_MOVE;
            if (t && t->kind == TYPE_ARRAY) {
                if (t->element_type) emit_type_name(t->element_type->kind, ctx->out);
                else fputs("int64_t", ctx->out);
                int arr_len = t->array_length > 0 ? t->array_length : 0;
                fprintf(ctx->out, " %.*s[%d]", (int)node->as.binding.name.length, node->as.binding.name.start,
                        arr_len);
                if (is_move && node->as.binding.expr->as.unary.right &&
                    node->as.binding.expr->as.unary.right->kind == AST_IDENTIFIER) {
                    fputs(";\n", ctx->out);
                    for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
                    fputs("memcpy(", ctx->out);
                    fprintf(ctx->out, "%.*s, ", (int)node->as.binding.name.length, node->as.binding.name.start);
                    AstNode *src = node->as.binding.expr->as.unary.right;
                    fprintf(ctx->out, "%.*s, sizeof(int64_t) * %d);\n",
                            (int)src->as.identifier.name.length, src->as.identifier.name.start, arr_len);
                } else {
                    fputs(" = ", ctx->out);
                    emit_expr(node->as.binding.expr, ctx);
                    fputs(";\n", ctx->out);
                }
            } else if (t && t->kind == TYPE_STRUCT && t->struct_name) {
                // M12.6: Struct binding
                fprintf(ctx->out, "%s %.*s", t->struct_name,
                        (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            } else if (t && t->kind == TYPE_OPTION) {
                // M8: Option binding
                fprintf(ctx->out, "TiqOption %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            } else if (t && t->kind == TYPE_RESULT) {
                // M8: Result binding
                fprintf(ctx->out, "TiqResult %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            } else {
                if (t) emit_type_name(t->kind, ctx->out);
                else fputs("int64_t", ctx->out);
                fprintf(ctx->out, " %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            }
            break;
        }
        case AST_ASSIGN:
            {
                if (node->as.assign.is_definition) {
                    SemanticType *st = node->semantic_type;
                    if (st && st->kind == TYPE_ARRAY) {
                        if (st->element_type) emit_type_name(st->element_type->kind, ctx->out);
                        else fputs("int64_t", ctx->out);
                        int arr_len = st->array_length > 0 ? st->array_length : 0;
                        fprintf(ctx->out, " %.*s[%d] = ", (int)node->as.assign.name.length, node->as.assign.name.start, arr_len);
                        emit_expr(node->as.assign.expr, ctx);
                        fputs(";\n", ctx->out);
                    } else {
                        if (st) emit_type_name(st->kind, ctx->out); else fputs("int64_t", ctx->out);
                        fprintf(ctx->out, " %.*s = ", (int)node->as.assign.name.length, node->as.assign.name.start);
                        emit_expr(node->as.assign.expr, ctx);
                        fputs(";\n", ctx->out);
                    }
                    break;
                }
                int arr_len = 0;
                SemanticType *st = node->semantic_type;
                if (st && st->kind == TYPE_ARRAY) arr_len = st->array_length;
                if (node->as.assign.index && arr_len > 0) {
                    fputs("if ((uint64_t)(", ctx->out);
                    emit_expr(node->as.assign.index, ctx);
                    fprintf(ctx->out, ") >= (uint64_t)(%d)) { ", arr_len);
                    fprintf(ctx->out, "fprintf(stderr, \"tiq: index out of bounds for array of length %d\\n\"); exit(1); }\n", arr_len);
                    for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
                }
                if (!node->as.assign.index && ref_param_kind(ctx, node->as.assign.name) != 0) {
                    // M9.1: assignment through a mutable borrow dereferences.
                    fprintf(ctx->out, "(*%.*s)", (int)node->as.assign.name.length, node->as.assign.name.start);
                } else {
                    fprintf(ctx->out, "%.*s", (int)node->as.assign.name.length, node->as.assign.name.start);
                }
                if (node->as.assign.index) {
                    fputs("[", ctx->out);
                    emit_expr(node->as.assign.index, ctx);
                    fputs("]", ctx->out);
                }
            }
            fputs(" ", ctx->out);
            {
                TokenKind op = node->as.assign.op;
                if (op == TOK_LARROW) fputs("=", ctx->out);
                else if (op == TOK_PLUS_EQ) fputs("+=", ctx->out);
                else if (op == TOK_MINUS_EQ) fputs("-=", ctx->out);
                else if (op == TOK_STAR_EQ) fputs("*=", ctx->out);
                else if (op == TOK_SLASH_EQ) fputs("/=", ctx->out);
                else if (op == TOK_PERCENT_EQ) fputs("%=", ctx->out);
                else fputs("=", ctx->out);
            }
            fputs(" ", ctx->out);
            emit_expr(node->as.assign.expr, ctx);
            fputs(";\n", ctx->out);
            break;
        case AST_FUNCTION:
            break;
        case AST_STRUCT_DEF:
            // M12.6: Struct definitions are emitted at the top level, not in statements
            break;
        case AST_BREAK:
            fputs("break;\n", ctx->out);
            break;
        case AST_SKIP:
            fputs("continue;\n", ctx->out);
            break;
        case AST_BRACKET_LOOP: {
            AstNode *domain = node->as.bracket_loop.domain;
            bool is_range = domain && domain->kind == AST_BINARY && domain->as.binary.op == TOK_DOT_DOT;
            if (is_range) {
                const char *var = node->as.bracket_loop.has_binder ?
                    node->as.bracket_loop.binder.start : "i";
                int var_len = node->as.bracket_loop.has_binder ?
                    (int)node->as.bracket_loop.binder.length : 1;
                fprintf(ctx->out, "for (int64_t %.*s = ", var_len, var);
                emit_expr(domain->as.binary.left, ctx);
                fprintf(ctx->out, "; %.*s < ", var_len, var);
                emit_expr(domain->as.binary.right, ctx);
                fprintf(ctx->out, "; %.*s++) {\n", var_len, var);
            } else {
                fputs("while (", ctx->out);
                emit_expr(domain, ctx);
                fputs(") {\n", ctx->out);
            }
            for (int i = 0; i < node->as.bracket_loop.body_count; i++) {
                emit_stmt(node->as.bracket_loop.body_stmts[i], ctx, indent + 1);
            }
            if (node->as.bracket_loop.body_final) {
                emit_stmt(node->as.bracket_loop.body_final, ctx, indent + 1);
            }
            for (int i = 0; i < indent; i++) fputs("    ", ctx->out);
            fputs("}\n", ctx->out);
            break;
        }
        case AST_BLOCK: {
            fputs("{\n", ctx->out);
            for (int i = 0; i < node->as.block.stmt_count; i++)
                emit_stmt(node->as.block.statements[i], ctx, indent + 1);
            if (node->as.block.final_expr)
                emit_stmt(node->as.block.final_expr, ctx, indent + 1);
            for (int i = node->as.block.defer_count - 1; i >= 0; i--)
                emit_stmt(node->as.block.deferred[i], ctx, indent + 1);
            for (int i = 0; i < indent; i++) fputs("    ", ctx->out);
            fputs("}\n", ctx->out);
            break;
        }
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINARY:
        case AST_CONDITIONAL: case AST_CALL:
        case AST_STREAM_GEN: case AST_ARRAY: case AST_ARRAY_FILL:
        case AST_FIELD_ACCESS: case AST_SPAWN: case AST_CHAN: case AST_MATCH:
        case AST_UNARY:
            emit_expr(node, ctx);
            fputs(";\n", ctx->out);
            break;
        case AST_DEFER:
            emit_stmt(node->as.defer.expr, ctx, indent);
            break;
        default:
            fputs(";\n", ctx->out);
            break;
    }
}

static void emit_check_node(AstNode *node, EmitContext *ctx);
static void emit_check_node(AstNode *node, EmitContext *ctx) {
    if (!node || ctx->diag->fatal_error) return;
    switch (node->kind) {
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINDING:
        case AST_ASSIGN: case AST_FUNCTION: case AST_BREAK: case AST_SKIP:
        case AST_ARRAY:
            break;
        case AST_BINARY:
            emit_check_node(node->as.binary.left, ctx);
            emit_check_node(node->as.binary.right, ctx);
            break;
        case AST_UNARY:
            emit_check_node(node->as.unary.right, ctx);
            break;
        case AST_CONDITIONAL:
            emit_check_node(node->as.conditional.cond, ctx);
            emit_check_node(node->as.conditional.then_branch, ctx);
            emit_check_node(node->as.conditional.else_branch, ctx);
            break;
        case AST_CALL:
            emit_check_node(node->as.call.callee, ctx);
            for (int i = 0; i < node->as.call.arg_count; i++)
                emit_check_node(node->as.call.args[i], ctx);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.stmt_count; i++)
                emit_check_node(node->as.block.statements[i], ctx);
            if (node->as.block.final_expr)
                emit_check_node(node->as.block.final_expr, ctx);
            break;
        case AST_BRACKET_LOOP:
            emit_check_node(node->as.bracket_loop.domain, ctx);
            for (int i = 0; i < node->as.bracket_loop.body_count; i++)
                emit_check_node(node->as.bracket_loop.body_stmts[i], ctx);
            if (node->as.bracket_loop.body_final)
                emit_check_node(node->as.bracket_loop.body_final, ctx);
            break;
        case AST_STREAM_GEN:
            for (int i = 0; i < node->as.stream_gen.seed_count; i++)
                emit_check_node(node->as.stream_gen.seeds[i], ctx);
            if (node->as.stream_gen.gen_expr)
                emit_check_node(node->as.stream_gen.gen_expr, ctx);
            if (node->as.stream_gen.bound)
                emit_check_node(node->as.stream_gen.bound, ctx);
            break;
        default:
            break;
    }
}

static void emit_stream_gen_def(const char *name, AstNode *node, Token *params, int param_count, EmitContext *ctx) {
    int sc = node->as.stream_gen.seed_count;
    if (sc == 1) {
        fprintf(ctx->out, "int64_t tiq_gen_%s(", name);
        for (int p = 0; p < param_count; p++) {
            if (p > 0) fputs(", ", ctx->out);
            fprintf(ctx->out, "int64_t %.*s", (int)params[p].length, params[p].start);
        }
        if (param_count > 0) fputs(", ", ctx->out);
        fputs("int64_t n) {\n", ctx->out);
        fputs("    if (n < 0) return 0;\n", ctx->out);
        fputs("    int64_t x = ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[0], ctx);
        fputs(";\n", ctx->out);
        fputs("    if (n == 0) return x;\n", ctx->out);
        fputs("    for (int64_t i = 1; i <= n; i++) {\n", ctx->out);
        fputs("        x = (", ctx->out);
        emit_expr(node->as.stream_gen.gen_expr, ctx);
        fputs(");\n", ctx->out);
        fputs("    }\n", ctx->out);
        fputs("    return x;\n", ctx->out);
        fputs("}\n\n", ctx->out);
    } else if (sc >= 2) {
        fprintf(ctx->out, "int64_t tiq_gen_%s(", name);
        for (int p = 0; p < param_count; p++) {
            if (p > 0) fputs(", ", ctx->out);
            fprintf(ctx->out, "int64_t %.*s", (int)params[p].length, params[p].start);
        }
        if (param_count > 0) fputs(", ", ctx->out);
        fputs("int64_t n) {\n", ctx->out);
        fputs("    if (n < 0) return 0;\n", ctx->out);
        fputs("    if (n == 0) return ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[0], ctx);
        fputs(";\n", ctx->out);
        fputs("    if (n == 1) return ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[1], ctx);
        fputs(";\n", ctx->out);
        fputs("    int64_t a = ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[1], ctx);
        fputs(";\n    int64_t b = ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[0], ctx);
        fputs(";\n", ctx->out);
        fputs("    for (int64_t i = 2; i <= n; i++) {\n", ctx->out);
        fputs("        int64_t t = (", ctx->out);
        emit_expr(node->as.stream_gen.gen_expr, ctx);
        fputs(");\n", ctx->out);
        fputs("        b = a;\n", ctx->out);
        fputs("        a = t;\n", ctx->out);
        fputs("    }\n", ctx->out);
        fputs("    return a;\n", ctx->out);
        fputs("}\n\n", ctx->out);
    }
}

void compile_to_c(const char *source_path, const char *source, FILE *out, DiagContext *diag) {
    EmitContext ectx = { out, diag, source_path, {{0, 0, 0}}, 0, NULL };
    EmitContext *ctx = &ectx;
    Parser parser;
    parser_init(&parser, source, source_path, diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    if (diag->has_error) { parser_free(&parser); return; }

    TypePool pool;
    type_pool_init(&pool);
    semantic_check(stmts, count, source_path, diag, &pool);
    if (diag->has_error) { parser_free(&parser); type_pool_free(&pool); return; }

    for (int i = 0; i < count && !diag->fatal_error; i++)
        emit_check_node(stmts[i], ctx);
    if (diag->has_error) { parser_free(&parser); type_pool_free(&pool); return; }

    // Collect stream gen bindings
    typedef struct { const char *name; AstNode *gen; Token *params; int param_count; } StreamGenDef;
    StreamGenDef stream_gens[TIQ_MAX_STREAM_GENS];
    int stream_gen_count = 0;
    for (int i = 0; i < count && stream_gen_count < TIQ_MAX_STREAM_GENS; i++) {
        if (stmts[i] && stmts[i]->kind == AST_BINDING &&
            stmts[i]->as.binding.expr && stmts[i]->as.binding.expr->kind == AST_STREAM_GEN) {
            Token n = stmts[i]->as.binding.name;
            char *sname = malloc(n.length + 1);
            memcpy(sname, n.start, n.length); sname[n.length] = '\0';
            stream_gens[stream_gen_count].name = sname;
            stream_gens[stream_gen_count].gen = stmts[i]->as.binding.expr;
            stream_gens[stream_gen_count].params = NULL;
            stream_gens[stream_gen_count].param_count = 0;
            stream_gen_count++;
        }
        // Also handle function-level streams
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION &&
            stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN &&
            stream_gen_count < TIQ_MAX_STREAM_GENS) {
            Token n = stmts[i]->as.function.name;
            char *sname = malloc(n.length + 1);
            memcpy(sname, n.start, n.length); sname[n.length] = '\0';
            stream_gens[stream_gen_count].name = sname;
            stream_gens[stream_gen_count].gen = stmts[i]->as.function.body;
            stream_gens[stream_gen_count].params = stmts[i]->as.function.params;
            stream_gens[stream_gen_count].param_count = stmts[i]->as.function.param_count;
            stream_gen_count++;
        }
    }

    int has_function = 0;
    for (int i = 0; i < count; i++)
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION) has_function = 1;

    // Populate emit-time stream gen lookup table
    ctx->stream_gen_count = 0;
    for (int g = 0; g < stream_gen_count && g < TIQ_MAX_STREAM_GENS; g++) {
        ctx->stream_gens[g].name = stream_gens[g].name;
        ctx->stream_gens[g].params = stream_gens[g].params;
        ctx->stream_gens[g].param_count = stream_gens[g].param_count;
        ctx->stream_gen_count++;
    }

    fputs(TIQ_RUNTIME_PRELUDE, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE2, ctx->out);

    // M12.6: Emit struct definitions (before function declarations so types are visible)
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind == AST_STRUCT_DEF) {
            SemanticType *st = stmts[i]->semantic_type;
            if (st && st->kind == TYPE_STRUCT && st->struct_name) {
                fprintf(ctx->out, "typedef struct {\n");
                for (int f = 0; f < st->field_count; f++) {
                    fputs("    ", ctx->out);
                    if (st->field_types[f]) emit_type_name(st->field_types[f]->kind, ctx->out);
                    else fputs("int64_t", ctx->out);
                    fprintf(ctx->out, " %s;\n", st->field_names[f]);
                }
                fprintf(ctx->out, "} %s;\n\n", st->struct_name);
            }
        }
    }

    // Forward-declare stream gen functions
    for (int g = 0; g < stream_gen_count; g++) {
        fprintf(ctx->out, "int64_t tiq_gen_%s(", stream_gens[g].name);
        for (int p = 0; p < stream_gens[g].param_count; p++) {
            if (p > 0) fputs(", ", ctx->out);
            fprintf(ctx->out, "int64_t %.*s", (int)stream_gens[g].params[p].length, stream_gens[g].params[p].start);
        }
        if (stream_gens[g].param_count > 0) fputs(", ", ctx->out);
        fputs("int64_t n);\n", ctx->out);
    }

    if (has_function) {
        for (int i = 0; i < count; i++) {
            if (stmts[i] && stmts[i]->kind == AST_FUNCTION &&
                !(stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN)) {
                SemanticType *t = stmts[i]->semantic_type;
                emit_semantic_type(t, ctx->out);
                fprintf(ctx->out, " %.*s(", (int)stmts[i]->as.function.name.length, stmts[i]->as.function.name.start);
                for (int j = 0; j < stmts[i]->as.function.param_count; j++) {
                    if (j > 0) fputs(", ", ctx->out);
                    SemanticType *pt = (SemanticType *)(stmts[i]->as.function.param_types ? stmts[i]->as.function.param_types[j] : NULL);
                    emit_semantic_type(pt, ctx->out);
                    fputs(" ", ctx->out);
                    fprintf(ctx->out, "%.*s", (int)stmts[i]->as.function.params[j].length, stmts[i]->as.function.params[j].start);
                }
                fputs(");\n", ctx->out);
            }
        }
    }

    fputs("\nint main(int argc, char **argv) {\n"
          "    tiq_argc = argc;\n"
          "    tiq_argv = argv;\n", ctx->out);

    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind != AST_FUNCTION && stmts[i]->kind != AST_STRUCT_DEF) {
            if (stmts[i]->kind == AST_BINDING && stmts[i]->as.binding.expr &&
                stmts[i]->as.binding.expr->kind == AST_STREAM_GEN) {
                // stream gen bindings are emitted via tiq_gen_* functions below
                continue;
            }
            emit_stmt(stmts[i], ctx, 1);
        }
    }
    fputs("    return 0;\n}\n\n", ctx->out);

    // Emit stream gen definitions
    for (int g = 0; g < stream_gen_count; g++)
        emit_stream_gen_def(stream_gens[g].name, stream_gens[g].gen, stream_gens[g].params, stream_gens[g].param_count, ctx);

    // Emit function definitions
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION && stmts[i]->as.function.body->kind != AST_STREAM_GEN) {
            ctx->current_fn = stmts[i]; // M9.1: body emission derefs ref params
            SemanticType *t = stmts[i]->semantic_type;
            emit_semantic_type(t, ctx->out);
            fprintf(ctx->out, "\n%.*s(", (int)stmts[i]->as.function.name.length, stmts[i]->as.function.name.start);
            for (int j = 0; j < stmts[i]->as.function.param_count; j++) {
                if (j > 0) fputs(", ", ctx->out);
                SemanticType *pt = (SemanticType *)(stmts[i]->as.function.param_types ? stmts[i]->as.function.param_types[j] : NULL);
                emit_semantic_type(pt, ctx->out);
                fputs(" ", ctx->out);
                fprintf(ctx->out, "%.*s", (int)stmts[i]->as.function.params[j].length, stmts[i]->as.function.params[j].start);
            }
            if (stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_BLOCK) {
                fputs(") {\n", ctx->out);
                AstNode *block = stmts[i]->as.function.body;
                for (int s = 0; s < block->as.block.stmt_count; s++) {
                    emit_stmt(block->as.block.statements[s], ctx, 1);
                }
                for (int d = 0; d < block->as.block.defer_count; d++) {
                    emit_stmt(block->as.block.deferred[d], ctx, 1);
                }
                if (block->as.block.final_expr) {
                    // M9.1: a trailing assignment/binding is a statement, not
                    // a value; emit it and return 0 (functions default to i64).
                    AstNode *fe = block->as.block.final_expr;
                    if (fe->kind == AST_ASSIGN || fe->kind == AST_BINDING) {
                        emit_stmt(fe, ctx, 1);
                        fputs("    return 0;\n", ctx->out);
                    } else {
                        fputs("    return ", ctx->out);
                        emit_expr(fe, ctx);
                        fputs(";\n", ctx->out);
                    }
                } else {
                    fputs("    return 0;\n", ctx->out);
                }
                fputs("}\n\n", ctx->out);
            } else {
                fputs(") {\n    return ", ctx->out);
                emit_expr(stmts[i]->as.function.body, ctx);
                fputs(";\n}\n\n", ctx->out);
            }
            ctx->current_fn = NULL;
        }
    }

    // Free stream gen names
    for (int g = 0; g < stream_gen_count; g++) free((void*)stream_gens[g].name);

    parser_free(&parser);
    type_pool_free(&pool);
}
