#define _POSIX_C_SOURCE 200809L
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
#include "../include/type.h"
#include "../include/emit_c.h"
#include "../include/module.h"

#define TIQ_VERSION "0.1.0-dev"

// M16.2: repeatable -l/-L link options forwarded to the host compiler
// (CLI.md). Capped; overflow fails closed with a usage error.
#define TIQ_MAX_LINK_OPTS 16

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


static int compile_file_to_c_stream(const char *input, FILE *out, DiagContext *diag) {
    // M13.1-P6: DFS-load the root file and its imports (canonical-path
    // dedupe, cycle detection), then compile the post-order module list
    // into a single C translation unit (LANGUAGE_SPEC §17.6).
    Program prog;
    if (!program_load(&prog, input, diag)) { program_free(&prog); return 1; }
    compile_modules_to_c(prog.sem, prog.count, input, out, diag);
    program_free(&prog);
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

static int run_host_compiler(const char *cc, const char *source_path, const char *output_path,
                             const char *target, char **link_opts, int link_count) {
    pid_t pid = fork();
    int status;
    if (pid < 0) { fprintf(stderr, "tiq: cannot start host C compiler: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        char target_arg[128] = "";
        char *args[16 + TIQ_MAX_LINK_OPTS * 2];
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
        for (int i = 0; i < link_count; i++)
            args[idx++] = link_opts[i];
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

static int build_target(const char *input, const char *output, const char *target,
                        char **link_opts, int link_count, DiagContext *diag) {
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
    result = run_host_compiler(cc, temp_name, output, target, link_opts, link_count);
    if (remove(temp_name) != 0) { fprintf(stderr, "tiq: cannot remove temporary C file %s: %s\n", temp_name, strerror(errno)); free(temp_name); return 1; }
    free(temp_name);
    return result;
}

static int build(const char *input, const char *output, char **link_opts, int link_count, DiagContext *diag) {
    return build_target(input, output, NULL, link_opts, link_count, diag);
}

// M16.2: parse repeatable `-l <lib>` / `-L <dir>` options starting at
// argv[from]; any other token fails closed with a usage error.
static int parse_link_opts(int argc, char **argv, int from,
                           char **link_buf, int *link_count) {
    for (int i = from; i < argc; i++) {
        if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0) && i + 1 < argc) {
            if (*link_count + 2 > TIQ_MAX_LINK_OPTS * 2)
                die("too many link options");
            link_buf[(*link_count)++] = argv[i];
            link_buf[(*link_count)++] = argv[++i];
        } else {
            return 0;
        }
    }
    return 1;
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
    parser_free(&parser); free(source);
    return diag->has_error ? 1 : 0;
}

static int dump_typed_ast(const char *input, DiagContext *diag) {
    // M15: resolve imports so cross-module symbols (e.g. std/ wrappers)
    // are visible; dump only the root module's non-import statements.
    Program prog;
    if (!program_load(&prog, input, diag)) {
        program_free(&prog);
        return 1;
    }
    TypePool pool;
    type_pool_init(&pool);
    if (!diag->has_error) semantic_check_modules(prog.sem, prog.count, diag, &pool);
    SemanticModule *root = &prog.sem[prog.count - 1];
    for (int i = 0; i < root->count; i++) ast_print(root->stmts[i], 0);
    program_free(&prog);
    type_pool_free(&pool);
    return diag->has_error ? 1 : 0;
}

static int cmd_check(const char *input) {
    DiagContext diag;
    diag_init(&diag);
    // M13.1-P6: checking resolves imports too, so cross-module symbols
    // are visible (flat namespace over the post-order module list).
    Program prog;
    if (!program_load(&prog, input, &diag)) {
        program_free(&prog);
        return 1;
    }
    TypePool pool;
    type_pool_init(&pool);
    semantic_check_modules(prog.sem, prog.count, &diag, &pool);
    program_free(&prog);
    type_pool_free(&pool);
    return diag.has_error ? 1 : 0;
}

static void usage(FILE *out) {
    fputs("tiq " TIQ_VERSION " - Tiq bootstrap compiler\n\n", out);
    fputs("usage:\n", out);
    fputs("  tiq --version\n", out);
    fputs("  tiq run <file.tiq> [-l lib] [-L dir]\n", out);
    fputs("  tiq build <file.tiq> [-o output] [--target <triple>] [-l lib] [-L dir]\n", out);
    fputs("  tiq emit-c <file.tiq>\n", out);
    fputs("  tiq check <file.tiq>...\n", out);
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
        if (strcmp(argv[1], "run") == 0) {
            if (argc < 3) { usage(stderr); return 2; }
            // Run file: build and execute
            const char *input = argv[2];
            char *link_buf[TIQ_MAX_LINK_OPTS * 2];
            int link_count = 0;
            if (!parse_link_opts(argc, argv, 3, link_buf, &link_count)) {
                usage(stderr);
                return 2;
            }
            char *tmp_exe = temporary_c_template();
            int fd = mkstemp(tmp_exe);
            if (fd < 0) { free(tmp_exe); return 1; }
            close(fd);
            DiagContext diag;
            diag_init(&diag);
            int result = build(input, tmp_exe, link_buf, link_count, &diag);
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
        char *link_buf[TIQ_MAX_LINK_OPTS * 2];
        int link_count = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                output = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                target = argv[++i];
            } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0) {
                if (i + 1 >= argc) { usage(stderr); return 2; }
                if (link_count + 2 > TIQ_MAX_LINK_OPTS * 2)
                    die("too many link options");
                link_buf[link_count++] = argv[i];
                link_buf[link_count++] = argv[++i];
            } else if (argv[i][0] != '-') {
                input = argv[i];
            }
        }
        if (!input) { usage(stderr); return 2; }
        return build_target(input, output, target, link_buf, link_count, &diag);
    }
    usage(stderr);
    return 2;
}
