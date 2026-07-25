#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "../include/lexer.h"
#include "../include/diag.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/formatter.h"
#include "../include/cache.h"
#include "../include/tester.h"
#include "../include/manifest.h"
#include "../include/lsp.h"
#include "../include/benchmark.h"

#define TIQ_VERSION "0.1.0-dev"

static void die(const char *message) {
    fprintf(stderr, "tiq: %s\n", message);
    exit(1);
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    long size = 0;
    char *data;
    if (file == NULL) { fprintf(stderr, "tiq: cannot open %s: %s\n", path, strerror(errno)); exit(1); }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); die("cannot measure source file"); }
    data = malloc((size_t)size + 1U);
    if (data == NULL) { fclose(file); die("out of memory"); }
    if (fread(data, 1U, (size_t)size, file) != (size_t)size) { free(data); fclose(file); die("cannot read source file"); }
    data[size] = '\0'; fclose(file);
    return data;
}

static void emit_c_string(FILE *out, const char *start, size_t length) {
    size_t i;
    fputc('"', out);
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)start[i];
        switch (ch) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (ch < 32U || ch == 127U) fprintf(out, "\\x%02x", ch);
                else fputc((int)ch, out);
        }
    }
    fputc('"', out);
}

static void emit_expr(AstNode *node, FILE *out, DiagContext *diag, const char *path);
static void emit_stmt(AstNode *node, FILE *out, DiagContext *diag, const char *path, int indent);

typedef struct { const char *name; Token *params; int param_count; } EmitStreamGenInfo;
static EmitStreamGenInfo emit_stream_gen_table[64];
static int emit_stream_gen_table_count = 0;

static bool is_stream_gen_name(const char *name, int len, int *out_params, int *out_param_count) {
    for (int i = 0; i < emit_stream_gen_table_count; i++) {
        if ((int)strlen(emit_stream_gen_table[i].name) == len &&
            memcmp(emit_stream_gen_table[i].name, name, len) == 0) {
            *out_params = 0;
            *out_param_count = emit_stream_gen_table[i].param_count;
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

static void emit_expr(AstNode *node, FILE *out, DiagContext *diag, const char *path) {
    if (!node) return;
    switch (node->kind) {
        case AST_LITERAL: {
            if (node->as.literal.type == TOK_INT || node->as.literal.type == TOK_FLOAT)
                fprintf(out, "%.*s", (int)node->token.length, node->token.start);
            else if (node->as.literal.type == TOK_STRING) emit_c_string(out, node->token.start + 1, node->token.length - 2);
            else if (node->as.literal.type == TOK_TRUE) fputs("1", out);
            else if (node->as.literal.type == TOK_FALSE) fputs("0", out);
            break;
        }
        case AST_IDENTIFIER:
            fprintf(out, "%.*s", (int)node->as.identifier.name.length, node->as.identifier.name.start);
            break;
        case AST_BINARY: {
            fputs("(", out);
            emit_expr(node->as.binary.left, out, diag, path);
            fputs(" ", out);
            fputs(binary_op_c_str(node->as.binary.op), out);
            fputs(" ", out);
            emit_expr(node->as.binary.right, out, diag, path);
            fputs(")", out);
            break;
        }
        case AST_UNARY: {
            if (node->as.unary.op == TOK_MOVE) {
                emit_expr(node->as.unary.right, out, diag, path);
            } else {
                const char *op = "";
                if (node->as.unary.op == TOK_BANG) op = "!";
                else if (node->as.unary.op == TOK_MINUS) op = "-";
                else if (node->as.unary.op == TOK_PLUS) op = "+";
                fputs(op, out); fputs("(", out);
                emit_expr(node->as.unary.right, out, diag, path);
                fputs(")", out);
            }
            break;
        }
        case AST_CONDITIONAL:
            emit_expr(node->as.conditional.cond, out, diag, path);
            fputs(" ? ", out);
            emit_expr(node->as.conditional.then_branch, out, diag, path);
            fputs(" : ", out);
            emit_expr(node->as.conditional.else_branch, out, diag, path);
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 3 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "len", 3) == 0) {
                SemanticType *ct = node->as.call.args[0] ?
                    node->as.call.args[0]->semantic_type : NULL;
                if (ct && ct->kind == TYPE_ARRAY) {
                    fprintf(out, "%d", ct->array_length);
                } else if (ct && (ct->kind == TYPE_SLICE || ct->kind == TYPE_STR_VIEW)) {
                    fputs("(((TiqSlice)", out);
                    emit_expr(node->as.call.args[0], out, diag, path);
                    fputs(").len)", out);
                } else if (ct && ct->kind == TYPE_STR) {
                    fputs("((int)strlen(", out);
                    emit_expr(node->as.call.args[0], out, diag, path);
                    fputs("))", out);
                } else {
                    fprintf(out, "0");
                }
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                const char *builtin_fn = NULL;
                if (name.length == 7 && memcmp(name.start, "fs_read", 7) == 0) builtin_fn = "tiq_fs_read";
                else if (name.length == 8 && memcmp(name.start, "fs_write", 8) == 0) builtin_fn = "tiq_fs_write";
                else if (name.length == 9 && memcmp(name.start, "fs_exists", 9) == 0) builtin_fn = "tiq_fs_exists";
                else if (name.length == 9 && memcmp(name.start, "proc_exec", 9) == 0) builtin_fn = "tiq_proc_exec";
                else if (name.length == 9 && memcmp(name.start, "proc_exit", 9) == 0) builtin_fn = "tiq_proc_exit";
                else if (name.length == 14 && memcmp(name.start, "json_parse_int", 14) == 0) builtin_fn = "tiq_json_parse_int";
                else if (name.length == 15 && memcmp(name.start, "json_encode_str", 15) == 0) builtin_fn = "tiq_json_encode_str";
                else if (name.length == 9 && memcmp(name.start, "net_fetch", 9) == 0) builtin_fn = "tiq_net_fetch";

                if (builtin_fn) {
                    fprintf(out, "%s(", builtin_fn);
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        if (i > 0) fputs(", ", out);
                        if (node->as.call.args[i]) emit_expr(node->as.call.args[i], out, diag, path);
                    }
                    fputs(")", out);
                    break;
                }
            }
            if (node->as.call.is_bracket_call && node->as.call.is_slice && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                fputs("((TiqSlice){ .ptr = ", out);
                if (ct && (ct->kind == TYPE_SLICE || ct->kind == TYPE_STR_VIEW)) {
                    fputs("((const char*)(((TiqSlice)", out);
                    emit_expr(node->as.call.callee, out, diag, path);
                    fputs(").ptr) + (", out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], out, diag, path);
                    else fputs("0", out);
                    fputs(") * ", out);
                    if (ct->kind == TYPE_SLICE) fputs("sizeof(int)", out);
                    else fputs("1", out);
                    fputs("), .len = (", out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], out, diag, path);
                    else {
                        fputs("(((TiqSlice)", out);
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs(").len)", out);
                    }
                    fputs(") - (", out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], out, diag, path);
                    else fputs("0", out);
                    fputs(") })", out);
                } else if (ct && ct->kind == TYPE_STR) {
                    fputs("((const char*)(", out);
                    emit_expr(node->as.call.callee, out, diag, path);
                    fputs(") + (", out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], out, diag, path);
                    else fputs("0", out);
                    fputs(")), .len = (", out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], out, diag, path);
                    else {
                        fputs("((int)strlen(", out);
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("))", out);
                    }
                    fputs(") - (", out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], out, diag, path);
                    else fputs("0", out);
                    fputs(") })", out);
                } else {
                    int arr_len = ct ? ct->array_length : 0;
                    fputs("((const char*)(", out);
                    emit_expr(node->as.call.callee, out, diag, path);
                    fputs(") + (", out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], out, diag, path);
                    else fputs("0", out);
                    fputs(") * sizeof(int)), .len = (", out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], out, diag, path);
                    else fprintf(out, "%d", arr_len);
                    fputs(") - (", out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], out, diag, path);
                    else fputs("0", out);
                    fputs(") })", out);
                }
                break;
            }
            if (node->as.call.is_bracket_call && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                if (ct && ct->kind == TYPE_ARRAY) {
                    int len = ct->array_length;
                    if (len > 0 && node->as.call.arg_count > 0 && node->as.call.args[0]) {
                        fputs("((unsigned)(", out);
                        emit_expr(node->as.call.args[0], out, diag, path);
                        fprintf(out, ") < (unsigned)(%d) ? ", len);
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("[", out);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (i > 0) fputs("][", out);
                            if (node->as.call.args[i]) emit_expr(node->as.call.args[i], out, diag, path);
                        }
                        fputs("] : (fprintf(stderr, \"tiq: index %d out of bounds for array of length %d\\n\", (int)(", out);
                        emit_expr(node->as.call.args[0], out, diag, path);
                        fprintf(out, "), %d), exit(1), ", len);
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("[", out);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (i > 0) fputs("][", out);
                            if (node->as.call.args[i]) emit_expr(node->as.call.args[i], out, diag, path);
                        }
                        fputs("]))", out);
                    } else {
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("[", out);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (i > 0) fputs("][", out);
                            if (node->as.call.args[i]) emit_expr(node->as.call.args[i], out, diag, path);
                        }
                        fputs("]", out);
                    }
                    break;
                }
                if (node->as.call.callee->kind == AST_IDENTIFIER) {
                    fprintf(out, "tiq_gen_%.*s(", (int)node->as.call.callee->as.identifier.name.length, node->as.call.callee->as.identifier.name.start);
                } else if (node->as.call.callee->kind == AST_CALL &&
                           node->as.call.callee->as.call.callee &&
                           node->as.call.callee->as.call.callee->kind == AST_IDENTIFIER) {
                    AstNode *inner_fn = node->as.call.callee->as.call.callee;
                    int dummy1, fn_param_count;
                    if (is_stream_gen_name(inner_fn->as.identifier.name.start,
                                           (int)inner_fn->as.identifier.name.length,
                                           &dummy1, &fn_param_count)) {
                        fprintf(out, "tiq_gen_%.*s(", (int)inner_fn->as.identifier.name.length, inner_fn->as.identifier.name.start);
                        for (int ai = 0; ai < node->as.call.callee->as.call.arg_count; ai++) {
                            if (ai > 0) fputs(", ", out);
                            if (node->as.call.callee->as.call.args[ai])
                                emit_expr(node->as.call.callee->as.call.args[ai], out, diag, path);
                        }
                        if (node->as.call.arg_count > 0) fputs(", ", out);
                    } else {
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("(", out);
                    }
                } else {
                    emit_expr(node->as.call.callee, out, diag, path);
                    fputs("(", out);
                }
            } else {
                emit_expr(node->as.call.callee, out, diag, path);
                fputs("(", out);
            }
            for (int i = 0; i < node->as.call.arg_count; i++) {
                if (i > 0) fputs(", ", out);
                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], out, diag, path);
            }
            fputs(")", out);
            break;
        case AST_BRACKET_EXPR:
            emit_expr(node->as.bracket_expr.expr, out, diag, path);
            break;
        case AST_ARRAY: {
            fputs("{", out);
            for (int i = 0; i < node->as.array.element_count; i++) {
                if (i > 0) fputs(", ", out);
                emit_expr(node->as.array.elements[i], out, diag, path);
            }
            fputs("}", out);
            break;
        }
        case AST_ARRAY_FILL:
            fputs("{ ", out);
            emit_expr(node->as.array_fill.value, out, diag, path);
            fputs(" }", out);
            break;
        case AST_FIELD_ACCESS:
            emit_expr(node->as.field_access.target, out, diag, path);
            fputs(".", out);
            fprintf(out, "%.*s", (int)node->as.field_access.field.length, node->as.field_access.field.start);
            break;
        case AST_SPAWN:
            fputs("/* spawn thread */ 0", out);
            break;
        case AST_CHAN:
            fputs("/* channel */ 0", out);
            break;
        case AST_MATCH: {
            fputs("(", out);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                fputs("(", out);
                emit_expr(node->as.match_expr.expr, out, diag, path);
                fputs(" == ", out);
                emit_expr(node->as.match_expr.arms[i].pattern, out, diag, path);
                fputs(") ? (", out);
                emit_expr(node->as.match_expr.arms[i].body, out, diag, path);
                fputs(") : ", out);
            }
            fputs("0)", out);
            break;
        }
        case AST_BLOCK:
            diag_error(diag, path, node->token.line, ERR_UNEXPECTED_TOKEN,
                       "block expression not supported in this context");
            break;
        case AST_DEFER:
            if (node->as.defer.expr)
                emit_expr(node->as.defer.expr, out, diag, path);
            break;
        default:
            break;
    }
}

static void emit_type_name(PrimitiveType kind, FILE *out) {
    switch (kind) {
        case TYPE_INT:      fputs("int", out); break;
        case TYPE_FLOAT:    fputs("double", out); break;
        case TYPE_BOOL:     fputs("int", out); break;
        case TYPE_STR:      fputs("const char *", out); break;
        case TYPE_ARRAY:    fputs("int", out); break;
        case TYPE_SLICE:    fputs("TiqSlice", out); break;
        case TYPE_STR_VIEW: fputs("TiqSlice", out); break;
        case TYPE_STREAM:   fputs("int", out); break;
        default:           fputs("int", out); break;
    }
}

static void emit_stmt(AstNode *node, FILE *out, DiagContext *diag, const char *path, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) fputs("    ", out);
    switch (node->kind) {
        case AST_BINDING: {
            SemanticType *t = node->semantic_type;
            bool is_move = node->as.binding.expr && node->as.binding.expr->kind == AST_UNARY &&
                           node->as.binding.expr->as.unary.op == TOK_MOVE;
            if (t && t->kind == TYPE_ARRAY) {
                if (t->element_type) emit_type_name(t->element_type->kind, out);
                else fputs("int", out);
                int arr_len = t->array_length > 0 ? t->array_length : 0;
                fprintf(out, " %.*s[%d]", (int)node->as.binding.name.length, node->as.binding.name.start,
                        arr_len);
                if (is_move && node->as.binding.expr->as.unary.right &&
                    node->as.binding.expr->as.unary.right->kind == AST_IDENTIFIER) {
                    fputs(";\n", out);
                    for (int j = 0; j < indent; j++) fputs("    ", out);
                    fputs("memcpy(", out);
                    fprintf(out, "%.*s, ", (int)node->as.binding.name.length, node->as.binding.name.start);
                    AstNode *src = node->as.binding.expr->as.unary.right;
                    fprintf(out, "%.*s, sizeof(int) * %d);\n",
                            (int)src->as.identifier.name.length, src->as.identifier.name.start, arr_len);
                } else {
                    fputs(" = ", out);
                    emit_expr(node->as.binding.expr, out, diag, path);
                    fputs(";\n", out);
                }
            } else {
                if (t) emit_type_name(t->kind, out);
                else fputs("int", out);
                fprintf(out, " %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", out);
                emit_expr(node->as.binding.expr, out, diag, path);
                fputs(";\n", out);
            }
            break;
        }
        case AST_ASSIGN:
            {
                int arr_len = 0;
                SemanticType *st = node->semantic_type;
                if (st && st->kind == TYPE_ARRAY) arr_len = st->array_length;
                if (node->as.assign.index && arr_len > 0) {
                    fputs("if ((unsigned)(", out);
                    emit_expr(node->as.assign.index, out, diag, path);
                    fprintf(out, ") >= (unsigned)(%d)) { ", arr_len);
                    fprintf(out, "fprintf(stderr, \"tiq: index out of bounds for array of length %d\\n\"); exit(1); }\n", arr_len);
                    for (int j = 0; j < indent; j++) fputs("    ", out);
                }
                fprintf(out, "%.*s", (int)node->as.assign.name.length, node->as.assign.name.start);
                if (node->as.assign.index) {
                    fputs("[", out);
                    emit_expr(node->as.assign.index, out, diag, path);
                    fputs("]", out);
                }
            }
            fputs(" ", out);
            {
                TokenKind op = node->as.assign.op;
                if (op == TOK_LARROW) fputs("=", out);
                else if (op == TOK_PLUS_EQ) fputs("+=", out);
                else if (op == TOK_MINUS_EQ) fputs("-=", out);
                else if (op == TOK_STAR_EQ) fputs("*=", out);
                else if (op == TOK_SLASH_EQ) fputs("/=", out);
                else if (op == TOK_PERCENT_EQ) fputs("%=", out);
                else fputs("=", out);
            }
            fputs(" ", out);
            emit_expr(node->as.assign.expr, out, diag, path);
            fputs(";\n", out);
            break;
        case AST_FUNCTION:
            break;
        case AST_BREAK:
            fputs("break;\n", out);
            break;
        case AST_SKIP:
            fputs("continue;\n", out);
            break;
        case AST_BRACKET_LOOP: {
            AstNode *domain = node->as.bracket_loop.domain;
            bool is_range = domain && domain->kind == AST_BINARY && domain->as.binary.op == TOK_DOT_DOT;
            if (is_range) {
                fprintf(out, "for (int i = ");
                emit_expr(domain->as.binary.left, out, diag, path);
                fprintf(out, "; i < ");
                emit_expr(domain->as.binary.right, out, diag, path);
                fputs("; i++) {\n", out);
            } else {
                fputs("while (", out);
                emit_expr(domain, out, diag, path);
                fputs(") {\n", out);
            }
            for (int i = 0; i < node->as.bracket_loop.body_count; i++) {
                emit_stmt(node->as.bracket_loop.body_stmts[i], out, diag, path, indent + 1);
            }
            if (node->as.bracket_loop.body_final) {
                emit_stmt(node->as.bracket_loop.body_final, out, diag, path, indent + 1);
            }
            for (int i = 0; i < indent; i++) fputs("    ", out);
            fputs("}\n", out);
            break;
        }
        case AST_BLOCK: {
            fputs("{\n", out);
            for (int i = 0; i < node->as.block.stmt_count; i++)
                emit_stmt(node->as.block.statements[i], out, diag, path, indent + 1);
            if (node->as.block.final_expr)
                emit_stmt(node->as.block.final_expr, out, diag, path, indent + 1);
            for (int i = node->as.block.defer_count - 1; i >= 0; i--)
                emit_stmt(node->as.block.deferred[i], out, diag, path, indent + 1);
            for (int i = 0; i < indent; i++) fputs("    ", out);
            fputs("}\n", out);
            break;
        }
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINARY:
        case AST_CONDITIONAL: case AST_CALL: case AST_BRACKET_EXPR:
        case AST_STREAM_GEN: case AST_ARRAY: case AST_ARRAY_FILL:
        case AST_FIELD_ACCESS: case AST_SPAWN: case AST_CHAN: case AST_MATCH:
            emit_expr(node, out, diag, path);
            fputs(";\n", out);
            break;
        case AST_UNARY:
            if (node->as.unary.op == TOK_BANG) {
                AstNode *expr = node->as.unary.right;
                SemanticType *st = expr ? expr->semantic_type : NULL;
                PrimitiveType kind = st ? st->kind : TYPE_INT;
                if (kind == TYPE_STR) {
                    fputs("printf(\"%s\\n\", ", out);
                    emit_expr(expr, out, diag, path);
                    fputs(");\n", out);
                } else if (kind == TYPE_FLOAT) {
                    fputs("printf(\"%g\\n\", (double)(", out);
                    emit_expr(expr, out, diag, path);
                    fputs("));\n", out);
                } else if (kind == TYPE_BOOL) {
                    fputs("printf(\"%s\\n\", (", out);
                    emit_expr(expr, out, diag, path);
                    fputs(") ? \"true\" : \"false\");\n", out);
                } else if (kind == TYPE_STR_VIEW || kind == TYPE_SLICE) {
                    fputs("printf(\"%.*s\\n\", ((TiqSlice)(", out);
                    emit_expr(expr, out, diag, path);
                    fputs(")).len, (const char*)(((TiqSlice)(", out);
                    emit_expr(expr, out, diag, path);
                    fputs(")).ptr));\n", out);
                } else {
                    fputs("printf(\"%d\\n\", (int)(", out);
                    emit_expr(expr, out, diag, path);
                    fputs("));\n", out);
                }
            } else {
                emit_expr(node, out, diag, path);
                fputs(";\n", out);
            }
            break;
        case AST_DEFER:
            emit_stmt(node->as.defer.expr, out, diag, path, indent);
            break;
        default:
            fputs(";\n", out);
            break;
    }
}

static void emit_check_node(AstNode *node, DiagContext *diag, const char *path);
static void emit_check_node(AstNode *node, DiagContext *diag, const char *path) {
    if (!node || diag->fatal_error) return;
    switch (node->kind) {
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINDING:
        case AST_ASSIGN: case AST_FUNCTION: case AST_BREAK: case AST_SKIP:
        case AST_BRACKET_EXPR: case AST_ARRAY:
            break;
        case AST_BINARY:
            emit_check_node(node->as.binary.left, diag, path);
            emit_check_node(node->as.binary.right, diag, path);
            break;
        case AST_UNARY:
            emit_check_node(node->as.unary.right, diag, path);
            break;
        case AST_CONDITIONAL:
            emit_check_node(node->as.conditional.cond, diag, path);
            emit_check_node(node->as.conditional.then_branch, diag, path);
            emit_check_node(node->as.conditional.else_branch, diag, path);
            break;
        case AST_CALL:
            emit_check_node(node->as.call.callee, diag, path);
            for (int i = 0; i < node->as.call.arg_count; i++)
                emit_check_node(node->as.call.args[i], diag, path);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.stmt_count; i++)
                emit_check_node(node->as.block.statements[i], diag, path);
            if (node->as.block.final_expr)
                emit_check_node(node->as.block.final_expr, diag, path);
            break;
        case AST_BRACKET_LOOP:
            emit_check_node(node->as.bracket_loop.domain, diag, path);
            for (int i = 0; i < node->as.bracket_loop.body_count; i++)
                emit_check_node(node->as.bracket_loop.body_stmts[i], diag, path);
            if (node->as.bracket_loop.body_final)
                emit_check_node(node->as.bracket_loop.body_final, diag, path);
            break;
        case AST_STREAM_GEN:
            for (int i = 0; i < node->as.stream_gen.seed_count; i++)
                emit_check_node(node->as.stream_gen.seeds[i], diag, path);
            if (node->as.stream_gen.gen_expr)
                emit_check_node(node->as.stream_gen.gen_expr, diag, path);
            if (node->as.stream_gen.bound)
                emit_check_node(node->as.stream_gen.bound, diag, path);
            break;
        default:
            break;
    }
}

static void emit_stream_gen_def(FILE *out, const char *name, AstNode *node, Token *params, int param_count, DiagContext *diag, const char *path) {
    int sc = node->as.stream_gen.seed_count;
    if (sc == 1) {
        fprintf(out, "int tiq_gen_%s(", name);
        for (int p = 0; p < param_count; p++) {
            if (p > 0) fputs(", ", out);
            fprintf(out, "int %.*s", (int)params[p].length, params[p].start);
        }
        if (param_count > 0) fputs(", ", out);
        fputs("int n) {\n", out);
        fputs("    if (n < 0) return 0;\n", out);
        fputs("    int x = ", out);
        emit_expr(node->as.stream_gen.seeds[0], out, diag, path);
        fputs(";\n", out);
        fputs("    if (n == 0) return x;\n", out);
        fputs("    int s = x;\n", out);
        fputs("    for (int i = 1; i <= n; i++) {\n", out);
        fputs("        x = (", out);
        emit_expr(node->as.stream_gen.gen_expr, out, diag, path);
        fputs(");\n", out);
        fputs("        s = x;\n", out);
        fputs("    }\n", out);
        fputs("    return x;\n", out);
        fputs("}\n\n", out);
    } else if (sc >= 2) {
        fprintf(out, "int tiq_gen_%s(", name);
        for (int p = 0; p < param_count; p++) {
            if (p > 0) fputs(", ", out);
            fprintf(out, "int %.*s", (int)params[p].length, params[p].start);
        }
        if (param_count > 0) fputs(", ", out);
        fputs("int n) {\n", out);
        fputs("    if (n < 0) return 0;\n", out);
        fputs("    if (n == 0) return ", out);
        emit_expr(node->as.stream_gen.seeds[0], out, diag, path);
        fputs(";\n", out);
        fputs("    if (n == 1) return ", out);
        emit_expr(node->as.stream_gen.seeds[1], out, diag, path);
        fputs(";\n", out);
        fputs("    int a = ", out);
        emit_expr(node->as.stream_gen.seeds[1], out, diag, path);
        fputs(";\n    int b = ", out);
        emit_expr(node->as.stream_gen.seeds[0], out, diag, path);
        fputs(";\n", out);
        fputs("    for (int i = 2; i <= n; i++) {\n", out);
        fputs("        int t = (", out);
        emit_expr(node->as.stream_gen.gen_expr, out, diag, path);
        fputs(");\n", out);
        fputs("        b = a;\n", out);
        fputs("        a = t;\n", out);
        fputs("    }\n", out);
        fputs("    return a;\n", out);
        fputs("}\n\n", out);
    }
}

static void compile_to_c(const char *source_path, const char *source, FILE *out, DiagContext *diag) {
    Parser parser;
    parser_init(&parser, source, source_path, diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    if (diag->has_error) { free(stmts); parser_free(&parser); return; }

    semantic_check(stmts, count, source_path, diag);
    if (diag->has_error) { free(stmts); parser_free(&parser); return; }

    for (int i = 0; i < count && !diag->fatal_error; i++)
        emit_check_node(stmts[i], diag, source_path);
    if (diag->has_error) { free(stmts); parser_free(&parser); return; }

    // Collect stream gen bindings
    typedef struct { const char *name; AstNode *gen; Token *params; int param_count; } StreamGenDef;
    StreamGenDef stream_gens[64];
    int stream_gen_count = 0;
    for (int i = 0; i < count; i++) {
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
            stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN) {
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
    emit_stream_gen_table_count = 0;
    for (int g = 0; g < stream_gen_count; g++) {
        emit_stream_gen_table[g].name = stream_gens[g].name;
        emit_stream_gen_table[g].params = stream_gens[g].params;
        emit_stream_gen_table[g].param_count = stream_gens[g].param_count;
        emit_stream_gen_table_count++;
    }

    fputs("#include <stdio.h>\n", out);
    fputs("#include <stdlib.h>\n", out);
    fputs("#include <string.h>\n", out);
    fputs("#include <sys/stat.h>\n", out);
    fputs("typedef struct { const void *ptr; int len; } TiqSlice;\n\n", out);

    fputs("static const char *tiq_fs_read(const char *path) {\n", out);
    fputs("    FILE *f = fopen(path, \"rb\");\n", out);
    fputs("    if (!f) return \"\";\n", out);
    fputs("    fseek(f, 0, SEEK_END);\n", out);
    fputs("    long len = ftell(f);\n", out);
    fputs("    fseek(f, 0, SEEK_SET);\n", out);
    fputs("    if (len < 0) { fclose(f); return \"\"; }\n", out);
    fputs("    char *buf = (char *)malloc(len + 1);\n", out);
    fputs("    if (!buf) { fclose(f); return \"\"; }\n", out);
    fputs("    size_t r = fread(buf, 1, len, f);\n", out);
    fputs("    fclose(f);\n", out);
    fputs("    buf[r] = '\\0';\n", out);
    fputs("    return buf;\n", out);
    fputs("}\n\n", out);

    fputs("static int tiq_fs_write(const char *path, const char *data) {\n", out);
    fputs("    FILE *f = fopen(path, \"wb\");\n", out);
    fputs("    if (!f) return -1;\n", out);
    fputs("    size_t len = strlen(data);\n", out);
    fputs("    size_t w = fwrite(data, 1, len, f);\n", out);
    fputs("    fclose(f);\n", out);
    fputs("    return w == len ? 0 : -1;\n", out);
    fputs("}\n\n", out);

    fputs("static int tiq_fs_exists(const char *path) {\n", out);
    fputs("    struct stat st;\n", out);
    fputs("    return stat(path, &st) == 0 ? 1 : 0;\n", out);
    fputs("}\n\n", out);

    fputs("static int tiq_proc_exec(const char *cmd) {\n", out);
    fputs("    return system(cmd);\n", out);
    fputs("}\n\n", out);

    fputs("static int tiq_proc_exit(int code) {\n", out);
    fputs("    exit(code);\n", out);
    fputs("    return 0;\n", out);
    fputs("}\n\n", out);

    fputs("static int tiq_json_parse_int(const char *str) {\n", out);
    fputs("    if (!str) return 0;\n", out);
    fputs("    return atoi(str);\n", out);
    fputs("}\n\n", out);

    fputs("static const char *tiq_json_encode_str(const char *str) {\n", out);
    fputs("    if (!str) return \"\\\"\\\"\";\n", out);
    fputs("    size_t len = strlen(str);\n", out);
    fputs("    char *buf = (char *)malloc(len * 2 + 3);\n", out);
    fputs("    if (!buf) return \"\\\"\\\"\";\n", out);
    fputs("    size_t pos = 0;\n", out);
    fputs("    buf[pos++] = '\"';\n", out);
    fputs("    for (size_t i = 0; i < len; i++) {\n", out);
    fputs("        if (str[i] == '\"') { buf[pos++] = '\\\\'; buf[pos++] = '\"'; }\n", out);
    fputs("        else if (str[i] == '\\\\') { buf[pos++] = '\\\\'; buf[pos++] = '\\\\'; }\n", out);
    fputs("        else if (str[i] == '\\n') { buf[pos++] = '\\\\'; buf[pos++] = 'n'; }\n", out);
    fputs("        else buf[pos++] = str[i];\n", out);
    fputs("    }\n", out);
    fputs("    buf[pos++] = '\"';\n", out);
    fputs("    buf[pos] = '\\0';\n", out);
    fputs("    return buf;\n", out);
    fputs("}\n\n", out);

    fputs("static const char *tiq_net_fetch(const char *url) {\n", out);
    fputs("    (void)url;\n", out);
    fputs("    return \"{\\\"status\\\": 200, \\\"ok\\\": true}\";\n", out);
    fputs("}\n\n", out);

    // Forward-declare stream gen functions
    for (int g = 0; g < stream_gen_count; g++) {
        fprintf(out, "int tiq_gen_%s(", stream_gens[g].name);
        for (int p = 0; p < stream_gens[g].param_count; p++) {
            if (p > 0) fputs(", ", out);
            fprintf(out, "int %.*s", (int)stream_gens[g].params[p].length, stream_gens[g].params[p].start);
        }
        if (stream_gens[g].param_count > 0) fputs(", ", out);
        fputs("int n);\n", out);
    }

    if (has_function) {
        for (int i = 0; i < count; i++) {
            if (stmts[i] && stmts[i]->kind == AST_FUNCTION &&
                !(stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN)) {
                SemanticType *t = stmts[i]->semantic_type;
                if (t) emit_type_name(t->kind, out); else fputs("int", out);
                fprintf(out, " %.*s(", (int)stmts[i]->as.function.name.length, stmts[i]->as.function.name.start);
                for (int j = 0; j < stmts[i]->as.function.param_count; j++) {
                    if (j > 0) fputs(", ", out);
                    fputs("int ", out);
                    fprintf(out, "%.*s", (int)stmts[i]->as.function.params[j].length, stmts[i]->as.function.params[j].start);
                }
                fputs(");\n", out);
            }
        }
    }
    fputs("\nint main(void) {\n", out);

    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind != AST_FUNCTION) {
            if (stmts[i]->kind == AST_BINDING && stmts[i]->as.binding.expr &&
                stmts[i]->as.binding.expr->kind == AST_STREAM_GEN) {
                // stream gen bindings are emitted via tiq_gen_* functions below
                continue;
            }
            emit_stmt(stmts[i], out, diag, source_path, 1);
        }
    }
    fputs("    return 0;\n}\n\n", out);

    // Emit stream gen definitions
    for (int g = 0; g < stream_gen_count; g++)
        emit_stream_gen_def(out, stream_gens[g].name, stream_gens[g].gen, stream_gens[g].params, stream_gens[g].param_count, diag, source_path);

    // Emit function definitions
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION && stmts[i]->as.function.body->kind != AST_STREAM_GEN) {
            SemanticType *t = stmts[i]->semantic_type;
            if (t) emit_type_name(t->kind, out); else fputs("int", out);
            fprintf(out, "\n%.*s(", (int)stmts[i]->as.function.name.length, stmts[i]->as.function.name.start);
            for (int j = 0; j < stmts[i]->as.function.param_count; j++) {
                if (j > 0) fputs(", ", out);
                fputs("int ", out);
                fprintf(out, "%.*s", (int)stmts[i]->as.function.params[j].length, stmts[i]->as.function.params[j].start);
            }
            fputs(") {\n    return ", out);
            emit_expr(stmts[i]->as.function.body, out, diag, source_path);
            fputs(";\n}\n\n", out);
        }
    }

    // Free stream gen names
    for (int g = 0; g < stream_gen_count; g++) free((void*)stream_gens[g].name);

    free(stmts);
    parser_free(&parser);
}

static int compile_file_to_c_stream(const char *input, FILE *out, DiagContext *diag) {
    char *source = read_all(input);
    compile_to_c(input, source, out, diag);
    free(source);
    if (diag->has_error) return 1;
    if (ferror(out)) { fprintf(stderr, "tiq: cannot write generated C: %s\n", strerror(errno)); return 1; }
    return 0;
}

static int emit_file(const char *input, const char *output, DiagContext *diag) {
    FILE *out = output == NULL ? stdout : fopen(output, "wb");
    int result;
    if (out == NULL) { fprintf(stderr, "tiq: cannot create %s: %s\n", output, strerror(errno)); return 1; }
    result = compile_file_to_c_stream(input, out, diag);
    if (output != NULL && fclose(out) != 0) { fprintf(stderr, "tiq: cannot close %s: %s\n", output, strerror(errno)); return 1; }
    if (output == NULL && fflush(out) != 0) { fprintf(stderr, "tiq: cannot flush generated C: %s\n", strerror(errno)); return 1; }
    if (result != 0) return 1;
    return 0;
}

static int run_host_compiler(const char *cc, const char *source_path, const char *output_path, const char *target) {
    pid_t pid = fork();
    int status;
    if (pid < 0) { fprintf(stderr, "tiq: cannot start host C compiler: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        char target_arg[128] = "";
        char *args[16];
        int idx = 0;
        args[idx++] = (char *)cc;
        args[idx++] = (char *)"-std=c11";
        args[idx++] = (char *)"-Os";
        args[idx++] = (char *)"-x";
        args[idx++] = (char *)"c";
        if (target && *target) {
            snprintf(target_arg, sizeof(target_arg), "--target=%s", target);
            args[idx++] = target_arg;
        }
        args[idx++] = (char *)source_path;
        args[idx++] = (char *)"-o";
        args[idx++] = (char *)output_path;
        args[idx] = NULL;
        execvp(cc, args);
        fprintf(stderr, "tiq: cannot execute host C compiler %s: %s\n", cc, strerror(errno));
        _exit(127);
    }
    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) break;
        if (errno != EINTR) { fprintf(stderr, "tiq: cannot wait for host C compiler: %s\n", strerror(errno)); return 1; }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { fprintf(stderr, "tiq: host C compiler failed\n"); return 1; }
    return 0;
}

static char *temporary_c_template(void) {
    const char *dir = getenv("TMPDIR");
    const char *suffix = "tiq-c-XXXXXX";
    size_t dir_len, suffix_len = strlen(suffix);
    int need_slash;
    char *path;
    if (dir == NULL || *dir == '\0') dir = "/tmp";
    dir_len = strlen(dir);
    need_slash = dir_len > 0U && dir[dir_len - 1U] != '/';
    path = malloc(dir_len + (need_slash ? 1U : 0U) + suffix_len + 1U);
    if (path == NULL) die("out of memory");
    memcpy(path, dir, dir_len);
    if (need_slash) { path[dir_len] = '/'; dir_len++; }
    memcpy(path + dir_len, suffix, suffix_len + 1U);
    return path;
}

static int build_target(const char *input, const char *output, const char *target, DiagContext *diag) {
    const char *cc = getenv("CC");
    char *temp_name = temporary_c_template();
    int fd, result;
    FILE *temp_file;
    if (cc == NULL || *cc == '\0') cc = "cc";
    fd = mkstemp(temp_name);
    if (fd < 0) { fprintf(stderr, "tiq: cannot create temporary C file: %s\n", strerror(errno)); free(temp_name); return 1; }
    temp_file = fdopen(fd, "wb");
    if (temp_file == NULL) { remove(temp_name); close(fd); fprintf(stderr, "tiq: cannot open temporary C file: %s\n", strerror(errno)); free(temp_name); return 1; }
    result = compile_file_to_c_stream(input, temp_file, diag);
    if (fclose(temp_file) != 0) { remove(temp_name); fprintf(stderr, "tiq: cannot close temporary C file: %s\n", strerror(errno)); free(temp_name); return 1; }
    if (result != 0) { remove(temp_name); free(temp_name); return 1; }
    result = run_host_compiler(cc, temp_name, output, target);
    if (remove(temp_name) != 0) { fprintf(stderr, "tiq: cannot remove temporary C file %s: %s\n", temp_name, strerror(errno)); free(temp_name); return 1; }
    free(temp_name);
    return result;
}

static int build(const char *input, const char *output, DiagContext *diag) {
    return build_target(input, output, NULL, diag);
}

static int dump_tokens(const char *input, DiagContext *diag) {
    char *source = read_all(input);
    Lexer lexer;
    lexer_init(&lexer, source, input, diag);
    for (;;) {
        Token token = lexer_next(&lexer);
        if (token.kind == TOK_EOF) break;
        if (token.kind == TOK_NEWLINE) continue;
        printf("%s", token_kind_name(token.kind));
        if (token.kind == TOK_IDENT || token.kind == TOK_INT || token.kind == TOK_FLOAT || token.kind == TOK_STRING)
            printf(" %.*s", (int)token.length, token.start);
        printf("\n");
    }
    free(source);
    return diag->has_error ? 1 : 0;
}

static int dump_ast(const char *input, DiagContext *diag) {
    char *source = read_all(input);
    Parser parser;
    parser_init(&parser, source, input, diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    for (int i = 0; i < count; i++) ast_print(stmts[i], 0);
    free(stmts); parser_free(&parser); free(source);
    return diag->has_error ? 1 : 0;
}

static int dump_typed_ast(const char *input, DiagContext *diag) {
    char *source = read_all(input);
    Parser parser;
    parser_init(&parser, source, input, diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    if (!diag->has_error) semantic_check(stmts, count, input, diag);
    for (int i = 0; i < count; i++) ast_print(stmts[i], 0);
    free(stmts); parser_free(&parser); free(source);
    return diag->has_error ? 1 : 0;
}

// ============================================================================
// Tooling commands (M5)
// ============================================================================

static int cmd_check(const char *input) {
    DiagContext diag;
    diag_init(&diag);
    char *source = read_all(input);
    Parser parser;
    parser_init(&parser, source, input, &diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    if (diag.has_error) {
        free(stmts);
        parser_free(&parser);
        free(source);
        return 1;
    }
    semantic_check(stmts, count, input, &diag);
    free(stmts);
    parser_free(&parser);
    free(source);
    return diag.has_error ? 1 : 0;
}

static int cmd_fmt(int argc, char **argv) {
    FormatterOptions opts;
    formatter_init_options(&opts);
    const char *output = NULL;
    bool check_mode = false;
    const char *input_file = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check_mode = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--use-tabs") == 0) {
            opts.use_tabs = true;
        } else if (strcmp(argv[i], "--indent-width") == 0 && i + 1 < argc) {
            opts.indent_width = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            // Input file
            input_file = argv[i];
        }
    }

    if (input_file) {
        int result = format_file(input_file, output, &opts);
        if (check_mode) {
            // TODO: Compare formatted output with original
        }
        return result;
    }

    // No input file, format stdin to stdout
    return format_stdin_to_file(output, &opts);
}

static int cmd_test(int argc, char **argv) {
    test_runner_init();
    TestResults results = {0, 0, 0};
    bool verbose = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
            // List mode - show files without running
            printf("Listing tests in: %s\n", argv[i]);
            // TODO: implement list mode
        } else if (argv[i][0] != '-') {
            struct stat st;
            if (stat(argv[i], &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    run_tests_in_dir(argv[i], &results);
                } else {
                    run_tests_in_file(argv[i], &results);
                }
            }
        }
    }

    test_runner_shutdown();

    printf("Tests: %d passed, %d failed, %d skipped\n",
           results.passed, results.failed, results.skipped);

    if (results.passed == 0 && results.failed == 0) {
        printf("Note: Test files should contain '//! expected_output' comments\n");
    }

    (void)verbose; // Reserved for future use
    return results.failed > 0 ? 1 : 0;
}

static int cmd_init(const char *name) {
    char manifest_path[256];
    if (name) {
        snprintf(manifest_path, sizeof(manifest_path), "%s.tiq.toml", name);
    } else {
        snprintf(manifest_path, sizeof(manifest_path), "tiq.toml");
    }

    FILE *f = fopen(manifest_path, "w");
    if (!f) {
        fprintf(stderr, "tiq: cannot create %s: %s\n", manifest_path, strerror(errno));
        return 1;
    }

    fprintf(f, "# Tiq package manifest\n");
    fprintf(f, "[package]\n");
    fprintf(f, "name = \"%s\"\n", name ? name : "my-package");
    fprintf(f, "version = \"0.1.0\"\n");
    fprintf(f, "description = \"A Tiq package\"\n");
    fprintf(f, "\n[tests]\n");
    fprintf(f, "dir = \"tests\"\n");

    fclose(f);
    printf("Created %s\n", manifest_path);
    return 0;
}

static int cmd_lsp(const char *root) {
    cache_init(NULL);
    return lsp_server_run(root ? root : ".", STDIN_FILENO, STDOUT_FILENO);
}

static int cmd_cache(int argc, char **argv) {
    cache_init(NULL);

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "clear") == 0) {
            cache_clear();
            printf("Cache cleared at %s\n", cache_get_path());
        } else if (strcmp(argv[i], "path") == 0) {
            printf("%s\n", cache_get_path());
        }
    }
    return 0;
}

static void usage(FILE *out) {
    fputs("tiq " TIQ_VERSION " - Tiq compiler and tools\n\n", out);
    fputs("usage:\n", out);
    fputs("  tiq --version\n", out);
    fputs("  tiq run <file.tiq>\n", out);
    fputs("  tiq build <file.tiq> [-o output]\n", out);
    fputs("  tiq emit-c <file.tiq>\n", out);
    fputs("  tiq check <file.tiq>...\n", out);
    fputs("  tiq fmt [--check] [--output <file>] [--use-tabs] [--indent-width <n>] [file]\n", out);
    fputs("  tiq test [--verbose] [--list] [dir|file...]\n", out);
    fputs("  tiq bench [-v] [-i N] [-q] <file|dir>...\n", out);
    fputs("  tiq init [name]\n", out);
    fputs("  tiq lsp [--root <path>]\n", out);
    fputs("  tiq cache [clear|path]\n", out);
    fputs("  tiq dump-tokens <file.tiq>\n", out);
    fputs("  tiq dump-ast <file.tiq>\n", out);
    fputs("  tiq dump-typed-ast <file.tiq>\n", out);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("tiq %s\n", TIQ_VERSION);
        return 0;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "check") == 0) {
            if (argc < 3) { usage(stderr); return 2; }
            int result = 0;
            for (int i = 2; i < argc; i++) {
                if (cmd_check(argv[i]) != 0) result = 1;
            }
            return result;
        }
        if (strcmp(argv[1], "fmt") == 0) {
            return cmd_fmt(argc, argv);
        }
        if (strcmp(argv[1], "test") == 0) {
            return cmd_test(argc, argv);
        }
        if (strcmp(argv[1], "init") == 0) {
            return cmd_init(argc > 2 ? argv[2] : NULL);
        }
        if (strcmp(argv[1], "lsp") == 0) {
            const char *root = NULL;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
                    root = argv[++i];
                }
            }
            return cmd_lsp(root);
        }
        if (strcmp(argv[1], "cache") == 0) {
            return cmd_cache(argc, argv);
        }
        if (strcmp(argv[1], "bench") == 0) {
            BenchmarkOptions opts;
            benchmark_init_options(&opts);
            const char *paths[64];
            int path_count = 0;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                    opts.verbose = true;
                } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
                    opts.quiet = true;
                } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                    opts.iterations = atoi(argv[++i]);
                } else if (argv[i][0] != '-') {
                    if (path_count < 64) paths[path_count++] = argv[i];
                }
            }
            if (path_count == 0) paths[path_count++] = ".";
            return benchmark_files(paths, path_count, &opts);
        }
        if (strcmp(argv[1], "run") == 0) {
            if (argc < 3) { usage(stderr); return 2; }
            // Run file: build and execute
            const char *input = argv[2];
            char *tmp_exe = temporary_c_template();
            int fd = mkstemp(tmp_exe);
            if (fd < 0) { free(tmp_exe); return 1; }
            close(fd);
            DiagContext diag;
            diag_init(&diag);
            int result = build(input, tmp_exe, &diag);
            if (result == 0) {
                // Execute
                pid_t pid = fork();
                if (pid == 0) {
                    execl(tmp_exe, tmp_exe, NULL);
                    _exit(127);
                }
                int status;
                waitpid(pid, &status, 0);
                result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
            remove(tmp_exe);
            free(tmp_exe);
            return result;
        }
    }

    DiagContext diag;
    diag_init(&diag);
    if (argc == 3 && strcmp(argv[1], "dump-tokens") == 0) return dump_tokens(argv[2], &diag);
    if (argc == 3 && strcmp(argv[1], "dump-ast") == 0) return dump_ast(argv[2], &diag);
    if (argc == 3 && strcmp(argv[1], "dump-typed-ast") == 0) return dump_typed_ast(argv[2], &diag);
    if (argc == 3 && strcmp(argv[1], "emit-c") == 0) return emit_file(argv[2], NULL, &diag);
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        const char *output = "a.out";
        const char *target = NULL;
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                output = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                target = argv[++i];
            } else if (argv[i][0] != '-') {
                input = argv[i];
            }
        }
        if (!input) { usage(stderr); return 2; }
        return build_target(input, output, target, &diag);
    }
    usage(stderr);
    return 2;
}
