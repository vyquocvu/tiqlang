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

#define TIQ_VERSION "0.1.0-dev"

static void die(const char *message) {
    fprintf(stderr, "tiq: %s\n", message);
    exit(1);
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *data;

    if (file == NULL) {
        fprintf(stderr, "tiq: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        die("cannot measure source file");
    }

    data = malloc((size_t)size + 1U);
    if (data == NULL) {
        fclose(file);
        die("out of memory");
    }
    if (fread(data, 1U, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        die("cannot read source file");
    }
    data[size] = '\0';
    fclose(file);
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
                if (ch < 32U || ch == 127U) {
                    fprintf(out, "\\x%02x", ch);
                } else {
                    fputc((int)ch, out);
                }
        }
    }
    fputc('"', out);
}


static void emit_ast_node(AstNode *node, FILE *out, DiagContext *diag, const char *source_path) {
    if (!node) return;

    if (node->kind == AST_PRINT) {
        AstNode *expr = node->as.print_stmt.expr;
        if (expr && expr->kind == AST_LITERAL && expr->as.literal.type == TOK_STRING) {
            if (out) {
                fputs("    fputs(", out);
                // We need to strip the quotes from the token for emit_c_string
                const char *start = expr->token.start + 1;
                size_t length = expr->token.length - 2;
                emit_c_string(out, start, length);
                fputs(", stdout);\n    fputc('\\n', stdout);\n", out);
            }
        } else {
            diag_error(diag, source_path, node->token.line, ERR_EXPECTED_STRING, "bootstrap compiler expects a string literal after '!'");
        }
    } else {
        // Any node other than PRINT is unsupported in M1 for C emission
        diag_error(diag, source_path, node->token.line, ERR_EXPECTED_PRINT, "expected print statement starting with '!'");
    }
}

static void compile_to_c(const char *source_path, const char *source, FILE *out, DiagContext *diag) {
    Parser parser;
    parser_init(&parser, source, source_path, diag);

    int count;
    AstNode **stmts = parser_parse(&parser, &count);

    if (diag->has_error) {
        free(stmts);
        parser_free(&parser);
        return;
    }

    for (int i = 0; i < count; i++) {
        // Look for errors during tree walking/emission since it's the backend that complains about unsupported stuff now
        emit_ast_node(stmts[i], NULL, diag, source_path);
    }

    if (diag->has_error) {
        free(stmts);
        parser_free(&parser);
        return;
    }

    fputs("#include <stdio.h>\n\nint main(void) {\n", out);

    for (int i = 0; i < count; i++) {
        emit_ast_node(stmts[i], out, diag, source_path);
    }

    fputs("    return 0;\n}\n", out);

    free(stmts);
    parser_free(&parser);
}

static int compile_file_to_c_stream(const char *input, FILE *out, DiagContext *diag) {
    char *source = read_all(input);

    compile_to_c(input, source, out, diag);
    free(source);

    if (diag->has_error) return 1;
    if (ferror(out)) {
        fprintf(stderr, "tiq: cannot write generated C: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int emit_file(const char *input, const char *output, DiagContext *diag) {
    FILE *out = output == NULL ? stdout : fopen(output, "wb");
    int result;

    if (out == NULL) {
        fprintf(stderr, "tiq: cannot create %s: %s\n", output, strerror(errno));
        return 1;
    }
    result = compile_file_to_c_stream(input, out, diag);
    if (output != NULL && fclose(out) != 0) {
        fprintf(stderr, "tiq: cannot close %s: %s\n", output, strerror(errno));
        return 1;
    }
    if (output == NULL && fflush(out) != 0) {
        fprintf(stderr, "tiq: cannot flush generated C: %s\n", strerror(errno));
        return 1;
    }
    if (result != 0) return 1;
    return 0;
}

static int run_host_compiler(const char *cc, const char *source_path, const char *output_path) {
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        fprintf(stderr, "tiq: cannot start host C compiler: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        char *const args[] = {
            (char *)cc,
            (char *)"-std=c11",
            (char *)"-Os",
            (char *)"-x",
            (char *)"c",
            (char *)source_path,
            (char *)"-o",
            (char *)output_path,
            NULL
        };
        execvp(cc, args);
        fprintf(stderr, "tiq: cannot execute host C compiler %s: %s\n", cc, strerror(errno));
        _exit(127);
    }

    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) break;
        if (errno != EINTR) {
            fprintf(stderr, "tiq: cannot wait for host C compiler: %s\n", strerror(errno));
            return 1;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "tiq: host C compiler failed\n");
        return 1;
    }
    return 0;
}

static char *temporary_c_template(void) {
    const char *dir = getenv("TMPDIR");
    const char *suffix = "tiq-c-XXXXXX";
    size_t dir_len;
    size_t suffix_len = strlen(suffix);
    int need_slash;
    char *path;

    if (dir == NULL || *dir == '\0') dir = "/tmp";
    dir_len = strlen(dir);
    need_slash = dir_len > 0U && dir[dir_len - 1U] != '/';
    path = malloc(dir_len + (need_slash ? 1U : 0U) + suffix_len + 1U);
    if (path == NULL) die("out of memory");
    memcpy(path, dir, dir_len);
    if (need_slash) {
        path[dir_len] = '/';
        dir_len++;
    }
    memcpy(path + dir_len, suffix, suffix_len + 1U);
    return path;
}

static int build(const char *input, const char *output, DiagContext *diag) {
    const char *cc = getenv("CC");
    char *temp_name = temporary_c_template();
    int fd;
    FILE *temp_file;
    int result;

    if (cc == NULL || *cc == '\0') cc = "cc";
    fd = mkstemp(temp_name);
    if (fd < 0) {
        fprintf(stderr, "tiq: cannot create temporary C file: %s\n", strerror(errno));
        free(temp_name);
        return 1;
    }
    temp_file = fdopen(fd, "wb");
    if (temp_file == NULL) {
        remove(temp_name);
        close(fd);
        fprintf(stderr, "tiq: cannot open temporary C file: %s\n", strerror(errno));
        free(temp_name);
        return 1;
    }
    result = compile_file_to_c_stream(input, temp_file, diag);
    if (fclose(temp_file) != 0) {
        remove(temp_name);
        fprintf(stderr, "tiq: cannot close temporary C file: %s\n", strerror(errno));
        free(temp_name);
        return 1;
    }
    if (result != 0) {
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    result = run_host_compiler(cc, temp_name, output);
    if (remove(temp_name) != 0) {
        fprintf(stderr, "tiq: cannot remove temporary C file %s: %s\n", temp_name, strerror(errno));
        free(temp_name);
        return 1;
    }
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
        if (token.kind == TOK_IDENT || token.kind == TOK_INT || token.kind == TOK_FLOAT || token.kind == TOK_STRING) {
            printf(" %.*s", (int)token.length, token.start);
        }
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

    for (int i = 0; i < count; i++) {
        ast_print(stmts[i], 0);
    }

    free(stmts);
    parser_free(&parser);
    free(source);

    return diag->has_error ? 1 : 0;
}

static void usage(FILE *out) {
    fputs("usage:\n"
          "  tiq --version\n"
          "  tiq dump-tokens <file.tiq>\n"
          "  tiq dump-ast <file.tiq>\n"
          "  tiq emit-c <file.tiq>\n"
          "  tiq build <file.tiq> [-o output]\n", out);
}

int main(int argc, char **argv) {
    DiagContext diag;
    diag_init(&diag);

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("tiq %s\n", TIQ_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "dump-tokens") == 0) {
        return dump_tokens(argv[2], &diag);
    }
    if (argc == 3 && strcmp(argv[1], "dump-ast") == 0) {
        return dump_ast(argv[2], &diag);
    }
    if (argc == 3 && strcmp(argv[1], "emit-c") == 0) {
        return emit_file(argv[2], NULL, &diag);
    }
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        const char *output = "a.out";
        if (argc == 5 && strcmp(argv[3], "-o") == 0) output = argv[4];
        else if (argc != 3) {
            usage(stderr);
            return 2;
        }
        return build(argv[2], output, &diag);
    }
    usage(stderr);
    return 2;
}
