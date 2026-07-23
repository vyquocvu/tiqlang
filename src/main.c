#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/lexer.h"
#include "../include/diag.h"
#include "../include/parser.h"
#include "../include/semantic.h"

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
            const char *op = "";
            if (node->as.unary.op == TOK_BANG) op = "!";
            else if (node->as.unary.op == TOK_MINUS) op = "-";
            else if (node->as.unary.op == TOK_PLUS) op = "+";
            fputs(op, out); fputs("(", out);
            emit_expr(node->as.unary.right, out, diag, path);
            fputs(")", out);
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
                } else {
                    fprintf(out, "0");
                }
                break;
            }
            if (node->as.call.is_bracket_call && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                if (ct && ct->kind == TYPE_ARRAY) {
                    int len = ct->array_length;
                    if (len > 0 && node->as.call.arg_count > 0) {
                        fputs("((unsigned)(", out);
                        emit_expr(node->as.call.args[0], out, diag, path);
                        fprintf(out, ") < (unsigned)(%d) ? ", len);
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("[", out);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (i > 0) fputs("][", out);
                            emit_expr(node->as.call.args[i], out, diag, path);
                        }
                        fputs("] : (fprintf(stderr, \"tiq: index %d out of bounds for array of length %d\\n\", (int)(", out);
                        emit_expr(node->as.call.args[0], out, diag, path);
                        fprintf(out, "), %d), exit(1), ", len);
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("[", out);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (i > 0) fputs("][", out);
                            emit_expr(node->as.call.args[i], out, diag, path);
                        }
                        fputs("]))", out);
                    } else {
                        emit_expr(node->as.call.callee, out, diag, path);
                        fputs("[", out);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (i > 0) fputs("][", out);
                            emit_expr(node->as.call.args[i], out, diag, path);
                        }
                        fputs("]", out);
                    }
                    break;
                }
                if (node->as.call.callee->kind == AST_IDENTIFIER) {
                    fprintf(out, "tiq_gen_%.*s(", (int)node->as.call.callee->as.identifier.name.length, node->as.call.callee->as.identifier.name.start);
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
                emit_expr(node->as.call.args[i], out, diag, path);
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
        case AST_BLOCK:
            diag_error(diag, path, node->token.line, ERR_UNEXPECTED_TOKEN,
                       "block expression not supported in this context");
            break;
        default:
            break;
    }
}

static void emit_type_name(PrimitiveType kind, FILE *out) {
    switch (kind) {
        case TYPE_INT:    fputs("int", out); break;
        case TYPE_FLOAT:  fputs("double", out); break;
        case TYPE_BOOL:   fputs("int", out); break;
        case TYPE_STR:    fputs("const char *", out); break;
        case TYPE_ARRAY:  fputs("int", out); break;
        default:          fputs("int", out); break;
    }
}

static void emit_stmt(AstNode *node, FILE *out, DiagContext *diag, const char *path, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) fputs("    ", out);
    switch (node->kind) {
        case AST_PRINT: {
            AstNode *expr = node->as.print_stmt.expr;
            if (!expr) { fputs(";\n", out); break; }
            SemanticType *t = expr->semantic_type;
            if (t && t->kind == TYPE_STR) {
                fputs("fputs(", out); emit_expr(expr, out, diag, path); fputs(", stdout);\n", out);
                for (int i = 0; i < indent; i++) fputs("    ", out);
                fputs("fputc('\\n', stdout);\n", out);
            } else if (t && t->kind == TYPE_INT) {
                fputs("printf(\"%d\\n\", (int)(", out); emit_expr(expr, out, diag, path); fputs("));\n", out);
            } else if (t && t->kind == TYPE_BOOL) {
                fputs("fputs(((", out); emit_expr(expr, out, diag, path);
                fputs(") ? \"true\" : \"false\"), stdout);\n", out);
                for (int i = 0; i < indent; i++) fputs("    ", out);
                fputs("fputc('\\n', stdout);\n", out);
            } else if (t && t->kind == TYPE_FLOAT) {
                fputs("printf(\"%g\\n\", (double)(", out); emit_expr(expr, out, diag, path); fputs("));\n", out);
            } else {
                fputs("printf(\"%d\\n\", (int)(", out); emit_expr(expr, out, diag, path); fputs("));\n", out);
            }
            break;
        }
        case AST_BINDING: {
            SemanticType *t = node->semantic_type;
            if (t && t->kind == TYPE_ARRAY) {
                if (t->element_type) emit_type_name(t->element_type->kind, out);
                else fputs("int", out);
                fprintf(out, " %.*s[%d] = ", (int)node->as.binding.name.length, node->as.binding.name.start,
                        t->array_length > 0 ? t->array_length : 0);
                emit_expr(node->as.binding.expr, out, diag, path);
                fputs(";\n", out);
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
            for (int i = 0; i < indent; i++) fputs("    ", out);
            fputs("}\n", out);
            break;
        }
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINARY: case AST_UNARY:
        case AST_CONDITIONAL: case AST_CALL: case AST_BRACKET_EXPR:
        case AST_STREAM_GEN: case AST_ARRAY:
            emit_expr(node, out, diag, path);
            fputs(";\n", out);
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
        case AST_LITERAL: case AST_IDENTIFIER: case AST_PRINT: case AST_BINDING:
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

static void emit_stream_gen_def(FILE *out, const char *name, AstNode *node, DiagContext *diag, const char *path) {
    int sc = node->as.stream_gen.seed_count;
    if (sc == 1) {
        fprintf(out, "int tiq_gen_%s(int n) {\n", name);
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
        fprintf(out, "int tiq_gen_%s(int n) {\n", name);
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
    typedef struct { const char *name; AstNode *gen; } StreamGenDef;
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
            stream_gen_count++;
        }
    }

    int has_function = 0;
    for (int i = 0; i < count; i++)
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION) has_function = 1;

    fputs("#include <stdio.h>\n", out);
    fputs("#include <stdlib.h>\n", out);

    // Forward-declare stream gen functions
    for (int g = 0; g < stream_gen_count; g++)
        fprintf(out, "int tiq_gen_%s(int n);\n", stream_gens[g].name);

    if (has_function) {
        for (int i = 0; i < count; i++) {
            if (stmts[i] && stmts[i]->kind == AST_FUNCTION) {
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
        emit_stream_gen_def(out, stream_gens[g].name, stream_gens[g].gen, diag, source_path);

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

static int run_host_compiler(const char *cc, const char *source_path, const char *output_path) {
    pid_t pid = fork();
    int status;
    if (pid < 0) { fprintf(stderr, "tiq: cannot start host C compiler: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        char *const args[] = { (char *)cc, (char *)"-std=c11", (char *)"-Os", (char *)"-x", (char *)"c",
                               (char *)source_path, (char *)"-o", (char *)output_path, NULL };
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

static int build(const char *input, const char *output, DiagContext *diag) {
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
    result = run_host_compiler(cc, temp_name, output);
    if (remove(temp_name) != 0) { fprintf(stderr, "tiq: cannot remove temporary C file %s: %s\n", temp_name, strerror(errno)); free(temp_name); return 1; }
    free(temp_name);
    return result;
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

static void usage(FILE *out) {
    fputs("usage:\n  tiq --version\n  tiq dump-tokens <file.tiq>\n  tiq dump-ast <file.tiq>\n"
          "  tiq dump-typed-ast <file.tiq>\n  tiq emit-c <file.tiq>\n  tiq build <file.tiq> [-o output]\n", out);
}

int main(int argc, char **argv) {
    DiagContext diag;
    diag_init(&diag);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { printf("tiq %s\n", TIQ_VERSION); return 0; }
    if (argc == 3 && strcmp(argv[1], "dump-tokens") == 0) return dump_tokens(argv[2], &diag);
    if (argc == 3 && strcmp(argv[1], "dump-ast") == 0) return dump_ast(argv[2], &diag);
    if (argc == 3 && strcmp(argv[1], "dump-typed-ast") == 0) return dump_typed_ast(argv[2], &diag);
    if (argc == 3 && strcmp(argv[1], "emit-c") == 0) return emit_file(argv[2], NULL, &diag);
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        const char *output = "a.out";
        if (argc == 5 && strcmp(argv[3], "-o") == 0) output = argv[4];
        else if (argc != 3) { usage(stderr); return 2; }
        return build(argv[2], output, &diag);
    }
    usage(stderr);
    return 2;
}
