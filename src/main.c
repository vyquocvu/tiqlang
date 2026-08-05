#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
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
#include "../include/ir.h"
#include "../include/emit_qbe.h"

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


static int compile_file_to_c_stream(const char *input, FILE *out, DiagContext *diag, int mode) {
    // M13.1-P6: DFS-load the root file and its imports (canonical-path
    // dedupe, cycle detection), then compile the post-order module list
    // into a single C translation unit (LANGUAGE_SPEC §17.6).
    Program prog;
    if (!program_load(&prog, input, diag)) { program_free(&prog); return 1; }
    compile_modules_to_c(prog.sem, prog.count, input, out, diag, mode);
    program_free(&prog);
    if (diag->has_error) return 1;
    if (ferror(out)) { fprintf(stderr, "tiq: cannot write generated C: %s\n", strerror(errno)); return 1; }
    return 0;
}

static int emit_file(const char *input, const char *output, DiagContext *diag, int mode) {
    FILE *out = output == NULL ? stdout : fopen(output, "wb");
    int result;
    if (out == NULL) { fprintf(stderr, "tiq: cannot create %s: %s\n", output, strerror(errno)); return 1; }
    result = compile_file_to_c_stream(input, out, diag, mode);
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

// M17.2: locate the directory containing the tiq executable so we can
// find sibling build artifacts (qbe, runtime_qbe.o).
static char *find_runtime_dir(void) {
    char resolved[4096];
#if defined(__APPLE__)
    extern int _NSGetExecutablePath(char *, uint32_t *);
    uint32_t sz = sizeof(resolved);
    if (_NSGetExecutablePath(resolved, &sz) != 0) return NULL;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (n < 0) return NULL;
    resolved[n] = '\0';
#else
    return NULL;
#endif
    char *last_slash = strrchr(resolved, '/');
    size_t dir_len;
    if (last_slash) dir_len = (size_t)(last_slash - resolved);
    else dir_len = strlen(resolved);
    // Check if qbe lives next to the tiq binary
    char check[4096];
    snprintf(check, sizeof(check), "%.*s/qbe", (int)dir_len, resolved);
    if (access(check, X_OK) == 0) {
        char *dir = malloc(dir_len + 1);
        if (!dir) die("out of memory");
        memcpy(dir, resolved, dir_len);
        dir[dir_len] = '\0';
        return dir;
    }
    // Fallback: walk up from cwd looking for build/qbe
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    for (int depth = 0; depth < 4; depth++) {
        snprintf(check, sizeof(check), "%s/build/qbe", cwd);
        if (access(check, X_OK) == 0) {
            size_t l = strlen(cwd);
            char *d = malloc(l + 7);
            if (!d) die("out of memory");
            memcpy(d, cwd, l); memcpy(d + l, "/build", 7);
            return d;
        }
        char *s = strrchr(cwd, '/');
        if (!s || s == cwd) break;
        *s = '\0';
    }
    return NULL;
}

// M17.2: temporary file name for QBE IL / object files.
static char *temporary_qbe_template(const char *suffix) {
    const char *dir = getenv("TMPDIR");
    const char *pfx = "tiq-qbe-";
    size_t dir_len, pfx_len = strlen(pfx), suf_len = strlen(suffix);
    char *path;
    int need_slash;
    if (!dir || !*dir) dir = "/tmp";
    dir_len = strlen(dir);
    need_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    path = malloc(dir_len + (need_slash ? 1 : 0) + pfx_len + suf_len + 1);
    if (!path) die("out of memory");
    memcpy(path, dir, dir_len);
    if (need_slash) path[dir_len++] = '/';
    memcpy(path + dir_len, pfx, pfx_len);
    memcpy(path + dir_len + pfx_len, suffix, suf_len + 1);
    return path;
}

// M17.2: post-process QBE's ARM64 assembly for macOS.
// QBE uses adrp/add/blr for external function calls, but the macOS
// linker requires direct `bl` for external symbols.  This function
// rewrites the pattern:
//   adrp  xN, _func@PAGE
//   add   xN, xN, #:lo12:_func@PAGEOFF
//   blr   xN
// into:
//   bl    _func
static void fixup_arm64_macho(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    char *out = malloc(sz * 2 + 1);
    if (!out) { free(buf); return; }
    char *wp = out;
    char *line = buf;

    // Buffer up to 3 lines for the adrp/add/blr pattern
    char saved[3][512];
    int nsaved = 0;
    char sym[256] = "";
    char adr_reg[16] = "";  // Register used in adrp/add
    int state = 0; // 0=none, 1=after adrp, 2=after adrp+add

    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        char tmp[512];
        if (llen >= sizeof(tmp)) llen = sizeof(tmp) - 1;
        memcpy(tmp, line, llen);
        tmp[llen] = '\0';

        char r1[16] = "", s1[256] = "";
        char r2[16] = "", r3[16] = "", s2[256] = "";
        char r4[16] = "";
        int matched = 0;

        if (state == 0 && sscanf(tmp, " adrp %15[^,], %255s", r1, s1) == 2 &&
            strstr(s1, "@PAGE")) {
            // Potential start of pattern
            snprintf(saved[0], sizeof(saved[0]), "%s", tmp);
            nsaved = 1;
            snprintf(sym, sizeof(sym), "%s", s1);
            char *at = strstr(sym, "@PAGE");
            if (at) *at = '\0';
            snprintf(adr_reg, sizeof(adr_reg), "%s", r1);
            state = 1;
            matched = 1;
        } else if (state == 1 && sscanf(tmp, " add %15[^,], %15[^,], #:lo12:%255s",
                                  r2, r3, s2) == 3 &&
            strcmp(r2, r3) == 0 && strcmp(r2, adr_reg) == 0 && strstr(s2, "@PAGEOFF")) {
            // Second line of pattern
            snprintf(saved[1], sizeof(saved[1]), "%s", tmp);
            nsaved = 2;
            state = 2;
            matched = 1;
        } else if (state == 2 && sscanf(tmp, " blr %15s", r4) == 1 &&
            strcmp(r4, adr_reg) == 0) {
            // Full pattern matched — emit bl instead
            wp += sprintf(wp, "\tbl\t%s\n", sym);
            state = 0;
            nsaved = 0;
            matched = 1;
        }

        if (matched) {
            line = nl ? nl + 1 : NULL;
            continue;
        }

        // Pattern broken — flush saved lines
        for (int i = 0; i < nsaved; i++) {
            size_t sl = strlen(saved[i]);
            memcpy(wp, saved[i], sl);
            wp[sl] = '\n';
            wp += sl + 1;
        }
        nsaved = 0;

        // Re-check current line as potential start of new pattern
        if (sscanf(tmp, " adrp %15[^,], %255s", r1, s1) == 2 && strstr(s1, "@PAGE")) {
            snprintf(saved[0], sizeof(saved[0]), "%s", tmp);
            nsaved = 1;
            snprintf(sym, sizeof(sym), "%s", s1);
            char *at = strstr(sym, "@PAGE");
            if (at) *at = '\0';
            snprintf(adr_reg, sizeof(adr_reg), "%s", r1);
            state = 1;
        } else {
            // Not a pattern start, emit current line
            memcpy(wp, tmp, llen);
            wp[llen] = '\n';
            wp += llen + 1;
            state = 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    // Flush any remaining saved lines
    for (int i = 0; i < nsaved; i++) {
        size_t sl = strlen(saved[i]);
        memcpy(wp, saved[i], sl);
        wp[sl] = '\n';
        wp += sl + 1;
    }
    *wp = '\0';
    free(buf);

    f = fopen(path, "w");
    if (f) { fputs(out, f); fclose(f); }
    free(out);
}

// M17.2: build via QBE backend.
// Pipeline: source -> IR -> QBE IL -> qbe .o -> link with runtime -> exe
static int build_qbe(const char *input, const char *output, DiagContext *diag) {
    // 1. Parse, resolve imports, semantic check
    Program prog;
    if (!program_load(&prog, input, diag)) { program_free(&prog); return 1; }
    TypePool pool;
    type_pool_init(&pool);
    if (!diag->has_error) semantic_check_modules(prog.sem, prog.count, diag, &pool);
    if (diag->has_error) {
        program_free(&prog); type_pool_free(&pool); return 1;
    }

    // 2. Lower to IR
    SemanticModule *root = &prog.sem[prog.count - 1];
    IrModule module;
    ir_module_init(&module);
    ir_lower(root->stmts, root->count, &module, diag);
    if (diag->has_error) {
        ir_module_free(&module); program_free(&prog); type_pool_free(&pool); return 1;
    }

    // 3. Emit QBE IL to temp file
    char *il_path = temporary_qbe_template("XXXXXX.il");
    int fd = mkstemp(il_path);
    if (fd < 0) {
        fprintf(stderr, "tiq: cannot create temp QBE IL file: %s\n", strerror(errno));
        free(il_path); ir_module_free(&module); program_free(&prog); type_pool_free(&pool);
        return 1;
    }
    FILE *il_file = fdopen(fd, "w");
    if (!il_file) {
        close(fd); remove(il_path); free(il_path);
        ir_module_free(&module); program_free(&prog); type_pool_free(&pool);
        return 1;
    }
    if (!emit_qbe(il_file, &module)) {
        fclose(il_file); remove(il_path); free(il_path);
        ir_module_free(&module); program_free(&prog); type_pool_free(&pool);
        fprintf(stderr, "tiq: QBE IL emission failed\n");
        return 1;
    }
    fclose(il_file);
    ir_module_free(&module);
    program_free(&prog);
    type_pool_free(&pool);

    // 4. Run QBE to produce .s (assembly)
    char *asm_path = temporary_qbe_template("XXXXXX.s");
    char *obj_path = temporary_qbe_template("XXXXXX.o");
    char *rt_dir = find_runtime_dir();
    if (!rt_dir) {
        fprintf(stderr, "tiq: cannot locate QBE runtime directory\n");
        remove(il_path); free(il_path); free(asm_path); free(obj_path);
        return 1;
    }
    char qbe_bin[4096], runtime_obj[4096];
    snprintf(qbe_bin, sizeof(qbe_bin), "%s/qbe", rt_dir);
    snprintf(runtime_obj, sizeof(runtime_obj), "%s/runtime_qbe.o", rt_dir);
    free(rt_dir);

    if (access(qbe_bin, X_OK) != 0) {
        fprintf(stderr, "tiq: QBE binary not found at %s\n", qbe_bin);
        remove(il_path); free(il_path); free(asm_path); free(obj_path);
        return 1;
    }

    pid_t pid = fork();
    int status;
    if (pid < 0) {
        fprintf(stderr, "tiq: fork: %s\n", strerror(errno));
        remove(il_path); free(il_path); free(asm_path); free(obj_path); return 1;
    }
    if (pid == 0) {
        execl(qbe_bin, qbe_bin, "-o", asm_path, il_path, NULL);
        fprintf(stderr, "tiq: cannot execute QBE: %s\n", strerror(errno));
        _exit(127);
    }
    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) break;
        if (errno != EINTR) {
            fprintf(stderr, "tiq: waitpid: %s\n", strerror(errno));
            remove(il_path); free(il_path); free(asm_path); free(obj_path); return 1;
        }
    }
    remove(il_path);
    free(il_path);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "tiq: QBE failed\n");
        remove(asm_path); free(asm_path); free(obj_path); return 1;
    }

    // 4b. Post-process assembly for ARM64 macOS (fix external call stubs)
#if defined(__APPLE__)
    fixup_arm64_macho(asm_path);
#endif

    // 4c. Assemble .s -> .o
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    pid_t apid = fork();
    if (apid < 0) {
        fprintf(stderr, "tiq: fork: %s\n", strerror(errno));
        remove(asm_path); free(asm_path); free(obj_path); return 1;
    }
    if (apid == 0) {
        execlp(cc, cc, "-c", asm_path, "-o", obj_path, NULL);
        fprintf(stderr, "tiq: cannot execute assembler: %s\n", strerror(errno));
        _exit(127);
    }
    for (;;) {
        if (waitpid(apid, &status, 0) >= 0) break;
        if (errno != EINTR) {
            fprintf(stderr, "tiq: waitpid: %s\n", strerror(errno));
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
    }
    remove(asm_path);
    free(asm_path);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "tiq: assembly failed\n");
        remove(obj_path); free(obj_path); return 1;
    }

    // 5. Link: cc obj_path runtime_obj -o output
    const char *cc2 = getenv("CC");
    if (!cc2 || !*cc2) cc2 = cc;
    pid_t lpid = fork();
    if (lpid < 0) {
        fprintf(stderr, "tiq: fork: %s\n", strerror(errno));
        remove(obj_path); free(obj_path); return 1;
    }
    if (lpid == 0) {
        execlp(cc2, cc2, obj_path, runtime_obj, "-o", output, NULL);
        fprintf(stderr, "tiq: cannot execute linker: %s\n", strerror(errno));
        _exit(127);
    }
    for (;;) {
        if (waitpid(lpid, &status, 0) >= 0) break;
        if (errno != EINTR) {
            fprintf(stderr, "tiq: waitpid: %s\n", strerror(errno));
            remove(obj_path); free(obj_path); return 1;
        }
    }
    remove(obj_path);
    free(obj_path);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "tiq: linking QBE output failed\n");
        return 1;
    }
    return 0;
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
    result = compile_file_to_c_stream(input, temp_file, diag, TIQ_EMIT_PROGRAM);
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

// M17.1: dump SSA-based IR for the typed AST.
static int dump_ir(const char *input, DiagContext *diag) {
    Program prog;
    if (!program_load(&prog, input, diag)) {
        program_free(&prog);
        return 1;
    }
    TypePool pool;
    type_pool_init(&pool);
    if (!diag->has_error) semantic_check_modules(prog.sem, prog.count, diag, &pool);
    if (diag->has_error) {
        program_free(&prog);
        type_pool_free(&pool);
        return 1;
    }
    SemanticModule *root = &prog.sem[prog.count - 1];
    IrModule module;
    ir_module_init(&module);
    ir_lower(root->stmts, root->count, &module, diag);
    if (!diag->has_error) ir_dump(stdout, &module);
    ir_module_free(&module);
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
    fputs("  tiq build <file.tiq> [-o output] [--backend qbe] [--target <triple>] [-l lib] [-L dir]\n", out);
    fputs("  tiq emit-c [--lib] <file.tiq>\n", out);
    fputs("  tiq emit-header <file.tiq> [-o output]\n", out);
    fputs("  tiq check <file.tiq>...\n", out);
    fputs("  tiq dump-tokens <file.tiq>\n", out);
    fputs("  tiq dump-ast <file.tiq>\n", out);
    fputs("  tiq dump-typed-ast <file.tiq>\n", out);
    fputs("  tiq dump-ir <file.tiq>\n", out);
    fputs("  tiq dump-qbe <file.tiq>\n", out);
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
            const char *backend = NULL;
            int run_from = 3;
            // M17.2: peek for --backend before link opts
            if (argc > 4 && strcmp(argv[3], "--backend") == 0) {
                backend = argv[4];
                run_from = 5;
            }
            if (!parse_link_opts(argc, argv, run_from, link_buf, &link_count)) {
                usage(stderr);
                return 2;
            }
            char *tmp_exe = temporary_c_template();
            int fd = mkstemp(tmp_exe);
            if (fd < 0) { free(tmp_exe); return 1; }
            close(fd);
            DiagContext diag;
            diag_init(&diag);
            int result;
            if (backend && strcmp(backend, "qbe") == 0) {
                result = build_qbe(input, tmp_exe, &diag);
            } else {
                result = build(input, tmp_exe, link_buf, link_count, &diag);
            }
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
    if (argc == 3 && strcmp(argv[1], "dump-ir") == 0) return dump_ir(argv[2], &diag);
    // M17.2: dump QBE IL for the typed AST (debugging / test inspection).
    if (argc == 3 && strcmp(argv[1], "dump-qbe") == 0) {
        Program prog;
        if (!program_load(&prog, argv[2], &diag)) { program_free(&prog); return 1; }
        TypePool pool;
        type_pool_init(&pool);
        if (!diag.has_error) semantic_check_modules(prog.sem, prog.count, &diag, &pool);
        if (diag.has_error) { program_free(&prog); type_pool_free(&pool); return 1; }
        SemanticModule *root = &prog.sem[prog.count - 1];
        IrModule module;
        ir_module_init(&module);
        ir_lower(root->stmts, root->count, &module, &diag);
        if (!diag.has_error) emit_qbe(stdout, &module);
        ir_module_free(&module);
        program_free(&prog);
        type_pool_free(&pool);
        return diag.has_error ? 1 : 0;
    }
    if (argc >= 3 && strcmp(argv[1], "emit-c") == 0) {
        // M16.3: `--lib` omits the entry point so the emitted C links
        // into a host program; the module graph must be definitions-only.
        int mode = TIQ_EMIT_PROGRAM;
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--lib") == 0) {
                mode = TIQ_EMIT_LIB;
            } else if (argv[i][0] != '-') {
                if (input) { usage(stderr); return 2; }
                input = argv[i];
            } else {
                usage(stderr);
                return 2;
            }
        }
        if (!input) { usage(stderr); return 2; }
        return emit_file(input, NULL, &diag, mode);
    }
    if (argc >= 3 && strcmp(argv[1], "emit-header") == 0) {
        // M16.3: C header for embedding a definitions-only Tiq library
        // into C/C++ projects (stdout by default, -o writes a file).
        const char *input = NULL;
        const char *output = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                output = argv[++i];
            } else if (argv[i][0] != '-') {
                if (input) { usage(stderr); return 2; }
                input = argv[i];
            } else {
                usage(stderr);
                return 2;
            }
        }
        if (!input) { usage(stderr); return 2; }
        return emit_file(input, output, &diag, TIQ_EMIT_HEADER);
    }
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        const char *output = "a.out";
        const char *target = NULL;
        const char *input = NULL;
        const char *backend = NULL;
        char *link_buf[TIQ_MAX_LINK_OPTS * 2];
        int link_count = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                output = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                target = argv[++i];
            } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
                backend = argv[++i];
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
        // M17.2: --backend qbe routes through the QBE native codegen path
        if (backend && strcmp(backend, "qbe") == 0) {
            return build_qbe(input, output, &diag);
        }
        return build_target(input, output, target, link_buf, link_count, &diag);
    }
    usage(stderr);
    return 2;
}
