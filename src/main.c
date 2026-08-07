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
#include "../include/emit_wasm.h"
#include "../include/asm_arm64.h"
#include "../include/asm_amd64.h"
#include "../include/asm_rv64.h"
#include "../include/macho_link.h"
#include "../include/elf_link.h"
#include "../include/pe_link.h"
#include <sys/stat.h>

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

// M17.3.2: link the program object with the QBE runtime into a self-signed
// executable via the integrated Mach-O linker (Darwin arm64 only). Returns 0
// on success; on failure prints a diagnostic and returns 1. The output file is
// written only after a successful link, so failures leave no partial binary.
#if defined(__APPLE__) && defined(__aarch64__)
static int link_qbe_objects(const char *obj_path, const char *runtime_obj,
                            const char *output) {
    uint8_t *blobs[2] = {NULL, NULL};
    MachOObject objs[2];
    memset(objs, 0, sizeof(objs));
    const char *paths[2] = {obj_path, runtime_obj};
    size_t loaded = 0;
    int result = 1;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", paths[i], strerror(errno));
            goto cleanup;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fprintf(stderr, "tiq: cannot read %s\n", paths[i]); goto cleanup; }
        long size = ftell(f);
        if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); fprintf(stderr, "tiq: cannot read %s\n", paths[i]); goto cleanup; }
        uint8_t *data = malloc((size_t)size);
        if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data); fclose(f);
            fprintf(stderr, "tiq: cannot read %s\n", paths[i]);
            goto cleanup;
        }
        fclose(f);
        blobs[i] = data;
        char err[256];
        if (macho_read(data, (size_t)size, &objs[i], err, sizeof(err)) != 0) {
            fprintf(stderr, "tiq: %s: %s\n", paths[i], err);
            goto cleanup;
        }
        loaded++;
    }
    {
        uint8_t *exe = NULL;
        size_t exe_len = 0;
        char err[512];
        if (link_macho_exec(objs, 2, "_main", &exe, &exe_len, err, sizeof(err)) != 0) {
            fprintf(stderr, "tiq: %s\n", err);
            goto cleanup;
        }
        FILE *out = fopen(output, "wb");
        int wrote = out && fwrite(exe, 1, exe_len, out) == exe_len && fclose(out) == 0;
        free(exe);
        if (!wrote) {
            if (out) fclose(out);
            fprintf(stderr, "tiq: cannot write %s\n", output);
            remove(output);
            goto cleanup;
        }
        chmod(output, 0755);
        result = 0;
    }
cleanup:
    for (size_t i = 0; i < loaded; i++) macho_object_free(&objs[i]);
    free(blobs[0]);
    free(blobs[1]);
    return result;
}
#endif

// M17.3.3/M17.3.4: link the program object with the QBE runtime into an ELF
// executable via the integrated ELF linker (Linux aarch64/x86_64).
#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
static int link_elf_objects(const char *obj_path, const char *runtime_obj,
                            const char *output) {
    uint8_t *blobs[2] = {NULL, NULL};
    ElfObject objs[2];
    memset(objs, 0, sizeof(objs));
    const char *paths[2] = {obj_path, runtime_obj};
    size_t loaded = 0;
    int result = 1;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", paths[i], strerror(errno));
            goto cleanup;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fprintf(stderr, "tiq: cannot read %s\n", paths[i]); goto cleanup; }
        long size = ftell(f);
        if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); fprintf(stderr, "tiq: cannot read %s\n", paths[i]); goto cleanup; }
        uint8_t *data = malloc((size_t)size);
        if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data); fclose(f);
            fprintf(stderr, "tiq: cannot read %s\n", paths[i]);
            goto cleanup;
        }
        fclose(f);
        blobs[i] = data;
        char err[256];
        if (elf_read(data, (size_t)size, &objs[i], err, sizeof(err)) != 0) {
            fprintf(stderr, "tiq: %s: %s\n", paths[i], err);
            goto cleanup;
        }
        loaded++;
    }
    {
        uint8_t *exe = NULL;
        size_t exe_len = 0;
        char err[512];
        if (link_elf_exec(objs, 2, "main", &exe, &exe_len, err, sizeof(err)) != 0) {
            fprintf(stderr, "tiq: %s\n", err);
            goto cleanup;
        }
        FILE *out = fopen(output, "wb");
        int wrote = out && fwrite(exe, 1, exe_len, out) == exe_len && fclose(out) == 0;
        free(exe);
        if (!wrote) {
            if (out) fclose(out);
            fprintf(stderr, "tiq: cannot write %s\n", output);
            remove(output);
            goto cleanup;
        }
        chmod(output, 0755);
        result = 0;
    }
cleanup:
    for (size_t i = 0; i < loaded; i++) elf_object_free(&objs[i]);
    free(blobs[0]);
    free(blobs[1]);
    return result;
}
#endif

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
    ir_lower(root->stmts, root->count, &module, diag, input);
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

    // 4b. Assemble .s -> .o (the integrated assembler lowers QBE's extern
    // call adrp/add/blr pattern to GOT relocations; no text fix-up needed)
#if defined(__APPLE__) && defined(__aarch64__)
    // M17.3.1: integrated Mach-O object writer — the QBE assembly subset
    // is assembled in-process; no external assembler is invoked.
    {
        FILE *asm_file = fopen(asm_path, "rb");
        if (!asm_file) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", asm_path, strerror(errno));
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fseek(asm_file, 0, SEEK_END);
        long asm_size = ftell(asm_file);
        fseek(asm_file, 0, SEEK_SET);
        char *asm_text = malloc((size_t)asm_size + 1);
        if (!asm_text || fread(asm_text, 1, (size_t)asm_size, asm_file) != (size_t)asm_size) {
            fclose(asm_file); free(asm_text);
            fprintf(stderr, "tiq: cannot read %s\n", asm_path);
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fclose(asm_file);
        AsmUnit unit;
        asm_unit_init(&unit);
        int asm_ok = asm_arm64_assemble(&unit, asm_text, (size_t)asm_size) == 0;
        if (asm_ok) {
            FILE *obj_file = fopen(obj_path, "wb");
            if (!obj_file || macho_obj_write(&unit, obj_file) != 0) asm_ok = 0;
            if (obj_file && fclose(obj_file) != 0) asm_ok = 0;
        }
        if (!asm_ok) {
            if (unit.has_error)
                fprintf(stderr, "tiq: %s:%d: error: %s\n", asm_path, unit.errline, unit.errmsg);
            else
                fprintf(stderr, "tiq: cannot write object file %s\n", obj_path);
            remove(obj_path);
            asm_unit_free(&unit); free(asm_text);
            remove(asm_path); free(asm_path); free(obj_path);
            return 1;
        }
        asm_unit_free(&unit);
        free(asm_text);
    }
    remove(asm_path);
    free(asm_path);
#elif defined(__linux__) && defined(__aarch64__)
    // M17.3.3: integrated ELF object writer — the QBE assembly subset
    // is assembled in-process with ELF format; no external assembler.
    {
        FILE *asm_file = fopen(asm_path, "rb");
        if (!asm_file) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", asm_path, strerror(errno));
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fseek(asm_file, 0, SEEK_END);
        long asm_size = ftell(asm_file);
        fseek(asm_file, 0, SEEK_SET);
        char *asm_text = malloc((size_t)asm_size + 1);
        if (!asm_text || fread(asm_text, 1, (size_t)asm_size, asm_file) != (size_t)asm_size) {
            fclose(asm_file); free(asm_text);
            fprintf(stderr, "tiq: cannot read %s\n", asm_path);
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fclose(asm_file);
        AsmUnit unit;
        asm_unit_init(&unit);
        unit.fmt = ASM_FMT_ELF;
        int asm_ok = asm_arm64_assemble(&unit, asm_text, (size_t)asm_size) == 0;
        if (asm_ok) {
            FILE *obj_file = fopen(obj_path, "wb");
            if (!obj_file || elf_obj_write(&unit, obj_file) != 0) asm_ok = 0;
            if (obj_file && fclose(obj_file) != 0) asm_ok = 0;
        }
        if (!asm_ok) {
            if (unit.has_error)
                fprintf(stderr, "tiq: %s:%d: error: %s\n", asm_path, unit.errline, unit.errmsg);
            else
                fprintf(stderr, "tiq: cannot write object file %s\n", obj_path);
            remove(obj_path);
            asm_unit_free(&unit); free(asm_text);
            remove(asm_path); free(asm_path); free(obj_path);
            return 1;
        }
        asm_unit_free(&unit);
        free(asm_text);
    }
    remove(asm_path);
    free(asm_path);
#elif defined(__linux__) && defined(__x86_64__)
    // M17.3.4: integrated ELF object writer for x86_64 — the QBE amd64
    // assembly subset is assembled in-process with ELF format.
    {
        FILE *asm_file = fopen(asm_path, "rb");
        if (!asm_file) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", asm_path, strerror(errno));
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fseek(asm_file, 0, SEEK_END);
        long asm_size = ftell(asm_file);
        fseek(asm_file, 0, SEEK_SET);
        char *asm_text = malloc((size_t)asm_size + 1);
        if (!asm_text || fread(asm_text, 1, (size_t)asm_size, asm_file) != (size_t)asm_size) {
            fclose(asm_file); free(asm_text);
            fprintf(stderr, "tiq: cannot read %s\n", asm_path);
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fclose(asm_file);
        AsmUnit unit;
        asm_unit_init(&unit);
        unit.fmt = ASM_FMT_ELF;
        int asm_ok = asm_amd64_assemble(&unit, asm_text, (size_t)asm_size) == 0;
        if (asm_ok) {
            FILE *obj_file = fopen(obj_path, "wb");
            if (!obj_file || elf_obj_write(&unit, obj_file) != 0) asm_ok = 0;
            if (obj_file && fclose(obj_file) != 0) asm_ok = 0;
        }
        if (!asm_ok) {
            if (unit.has_error)
                fprintf(stderr, "tiq: %s:%d: error: %s\n", asm_path, unit.errline, unit.errmsg);
            else
                fprintf(stderr, "tiq: cannot write object file %s\n", obj_path);
            remove(obj_path);
            asm_unit_free(&unit); free(asm_text);
            remove(asm_path); free(asm_path); free(obj_path);
            return 1;
        }
        asm_unit_free(&unit);
        free(asm_text);
    }
    remove(asm_path);
    free(asm_path);
#elif defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
    // M17.4.1: integrated ELF object writer for riscv64 — the QBE rv64
    // assembly subset is assembled in-process with ELF format.
    {
        FILE *asm_file = fopen(asm_path, "rb");
        if (!asm_file) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", asm_path, strerror(errno));
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fseek(asm_file, 0, SEEK_END);
        long asm_size = ftell(asm_file);
        fseek(asm_file, 0, SEEK_SET);
        char *asm_text = malloc((size_t)asm_size + 1);
        if (!asm_text || fread(asm_text, 1, (size_t)asm_size, asm_file) != (size_t)asm_size) {
            fclose(asm_file); free(asm_text);
            fprintf(stderr, "tiq: cannot read %s\n", asm_path);
            remove(asm_path); free(asm_path); free(obj_path); return 1;
        }
        fclose(asm_file);
        AsmUnit unit;
        asm_unit_init(&unit);
        unit.fmt = ASM_FMT_ELF;
        int asm_ok = asm_rv64_assemble(&unit, asm_text, (size_t)asm_size) == 0;
        if (asm_ok) {
            FILE *obj_file = fopen(obj_path, "wb");
            if (!obj_file || elf_obj_write(&unit, obj_file) != 0) asm_ok = 0;
            if (obj_file && fclose(obj_file) != 0) asm_ok = 0;
        }
        if (!asm_ok) {
            if (unit.has_error)
                fprintf(stderr, "tiq: %s:%d: error: %s\n", asm_path, unit.errline, unit.errmsg);
            else
                fprintf(stderr, "tiq: cannot write object file %s\n", obj_path);
            remove(obj_path);
            asm_unit_free(&unit); free(asm_text);
            remove(asm_path); free(asm_path); free(obj_path);
            return 1;
        }
        asm_unit_free(&unit);
        free(asm_text);
    }
    remove(asm_path);
    free(asm_path);
#else
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
#endif

    // 5. Link the program object with the QBE runtime.
    //    Default on Darwin arm64: the integrated Mach-O linker emits a
    //    self-signed executable entirely in-process. TIQ_QBE_LINK=<cmd> is an
    //    escape hatch that links through an external toolchain (e.g. `cc`).
    const char *qlink = getenv("TIQ_QBE_LINK");
#if defined(__APPLE__) && defined(__aarch64__)
    if (!qlink || !*qlink) {
        int lr = link_qbe_objects(obj_path, runtime_obj, output);
        remove(obj_path);
        free(obj_path);
        return lr;
    }
#endif
#if defined(__linux__) && defined(__aarch64__)
    if (!qlink || !*qlink) {
        int lr = link_elf_objects(obj_path, runtime_obj, output);
        remove(obj_path);
        free(obj_path);
        return lr;
    }
#endif
#if defined(__linux__) && defined(__x86_64__)
    if (!qlink || !*qlink) {
        int lr = link_elf_objects(obj_path, runtime_obj, output);
        remove(obj_path);
        free(obj_path);
        return lr;
    }
#endif
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
    if (!qlink || !*qlink) {
        int lr = link_elf_objects(obj_path, runtime_obj, output);
        remove(obj_path);
        free(obj_path);
        return lr;
    }
#endif
    {
        const char *linker = (qlink && *qlink) ? qlink : "cc";
        pid_t lpid = fork();
        if (lpid < 0) {
            fprintf(stderr, "tiq: fork: %s\n", strerror(errno));
            remove(obj_path); free(obj_path); return 1;
        }
        if (lpid == 0) {
            execlp(linker, linker, obj_path, runtime_obj, "-o", output, NULL);
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

// M17.4.2: build via the wasm32-wasi backend.
// Pipeline: source -> IR -> in-process wasm32 module (no external tools).
static int build_wasm(const char *input, const char *output, DiagContext *diag) {
    Program prog;
    if (!program_load(&prog, input, diag)) { program_free(&prog); return 1; }
    TypePool pool;
    type_pool_init(&pool);
    if (!diag->has_error) semantic_check_modules(prog.sem, prog.count, diag, &pool);
    if (diag->has_error) {
        program_free(&prog); type_pool_free(&pool); return 1;
    }

    SemanticModule *root = &prog.sem[prog.count - 1];
    IrModule module;
    ir_module_init(&module);
    ir_lower(root->stmts, root->count, &module, diag, input);
    if (diag->has_error) {
        ir_module_free(&module); program_free(&prog); type_pool_free(&pool); return 1;
    }

    uint8_t *wasm = NULL;
    size_t wasm_len = 0;
    char err[256];
    bool ok = emit_wasm(&module, &wasm, &wasm_len, err, sizeof(err));
    ir_module_free(&module);
    program_free(&prog);
    type_pool_free(&pool);
    if (!ok) {
        fprintf(stderr, "tiq: %s\n", err);
        return 1;
    }

    FILE *out = fopen(output, "wb");
    if (out == NULL) {
        fprintf(stderr, "tiq: cannot create %s: %s\n", output, strerror(errno));
        free(wasm); return 1;
    }
    int result = 0;
    if (fwrite(wasm, 1, wasm_len, out) != wasm_len || fclose(out) != 0) {
        fprintf(stderr, "tiq: cannot write %s\n", output);
        result = 1;
    }
    free(wasm);
    return result;
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
    ir_lower(root->stmts, root->count, &module, diag, input);
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

// M17.3.1/M17.3.3/M17.3.4: assemble the QBE assembly subset in-process and
// write a relocatable object (Mach-O on Darwin arm64, ELF64 on Linux aarch64/x86_64).
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__)))
static void usage(FILE *out);
static int cmd_emit_obj(const char *input, const char *output) {
    FILE *f = fopen(input, "rb");
    if (!f) {
        fprintf(stderr, "tiq: cannot read %s: %s\n", input, strerror(errno));
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc((size_t)size + 1);
    if (!source || fread(source, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(source);
        fprintf(stderr, "tiq: cannot read %s\n", input);
        return 1;
    }
    fclose(f);
    AsmUnit unit;
    asm_unit_init(&unit);
#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__) || (defined(__riscv) && __riscv_xlen == 64))
    unit.fmt = ASM_FMT_ELF;
#endif
#if defined(__x86_64__)
    if (asm_amd64_assemble(&unit, source, (size_t)size) != 0) {
#elif defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
    if (asm_rv64_assemble(&unit, source, (size_t)size) != 0) {
#else
    if (asm_arm64_assemble(&unit, source, (size_t)size) != 0) {
#endif
        fprintf(stderr, "tiq: %s:%d: error: %s\n", input, unit.errline, unit.errmsg);
        asm_unit_free(&unit); free(source);
        return 1;
    }
    FILE *out = fopen(output, "wb");
    int result;
#if defined(__APPLE__) && defined(__aarch64__)
    result = out ? macho_obj_write(&unit, out) : 1;
#elif defined(__x86_64__)
    result = out ? elf_obj_write(&unit, out) : 1;
#else
    result = out ? elf_obj_write(&unit, out) : 1;
#endif
    if (out && fclose(out) != 0) result = 1;
    if (result != 0) {
        fprintf(stderr, "tiq: cannot write %s\n", output);
        remove(output);
    }
    asm_unit_free(&unit);
    free(source);
    return result;
}

// M17.3.2/M17.3.3/M17.3.4: link relocatable objects into an executable entirely
// in-process (Mach-O on Darwin arm64, ELF64 on Linux aarch64/x86_64).
static int cmd_link_qbe(int argc, char **argv) {
    const char *inputs[TIQ_MAX_LINK_OPTS];
    int ninputs = 0;
    const char *output = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] != '-') {
            if (ninputs >= TIQ_MAX_LINK_OPTS) { usage(stderr); return 2; }
            inputs[ninputs++] = argv[i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (ninputs == 0 || !output) { usage(stderr); return 2; }

#if defined(__APPLE__) && defined(__aarch64__)
    MachOObject *objs = calloc((size_t)ninputs, sizeof(MachOObject));
#elif defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
    ElfObject *objs = calloc((size_t)ninputs, sizeof(ElfObject));
#endif
    uint8_t **blobs = calloc((size_t)ninputs, sizeof(uint8_t *));
    if (!objs || !blobs) die("out of memory");
    size_t loaded = 0;
    int result = 1;
    for (int i = 0; i < ninputs; i++) {
        FILE *f = fopen(inputs[i], "rb");
        if (!f) {
            fprintf(stderr, "tiq: cannot read %s: %s\n", inputs[i], strerror(errno));
            goto cleanup;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fprintf(stderr, "tiq: cannot read %s\n", inputs[i]); goto cleanup; }
        long size = ftell(f);
        if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); fprintf(stderr, "tiq: cannot read %s\n", inputs[i]); goto cleanup; }
        uint8_t *data = malloc((size_t)size);
        if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data); fclose(f);
            fprintf(stderr, "tiq: cannot read %s\n", inputs[i]);
            goto cleanup;
        }
        fclose(f);
        blobs[i] = data;
        char err[256];
#if defined(__APPLE__) && defined(__aarch64__)
        if (macho_read(data, (size_t)size, &objs[i], err, sizeof(err)) != 0) {
#elif defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
        if (elf_read(data, (size_t)size, &objs[i], err, sizeof(err)) != 0) {
#endif
            fprintf(stderr, "tiq: %s: %s\n", inputs[i], err);
            goto cleanup;
        }
        loaded++;
    }
    {
        uint8_t *exe = NULL;
        size_t exe_len = 0;
        char err[512];
#if defined(__APPLE__) && defined(__aarch64__)
        if (link_macho_exec(objs, (size_t)ninputs, "_main", &exe, &exe_len,
                            err, sizeof(err)) != 0) {
#elif defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
        if (link_elf_exec(objs, (size_t)ninputs, "main", &exe, &exe_len,
                          err, sizeof(err)) != 0) {
#endif
            fprintf(stderr, "tiq: %s\n", err);
            goto cleanup;
        }
        FILE *out = fopen(output, "wb");
        int wrote = out && fwrite(exe, 1, exe_len, out) == exe_len &&
                    fclose(out) == 0;
        free(exe);
        if (!wrote) {
            if (out) fclose(out);
            fprintf(stderr, "tiq: cannot write %s\n", output);
            remove(output);
            goto cleanup;
        }
        chmod(output, 0755);
        result = 0;
    }
cleanup:
#if defined(__APPLE__) && defined(__aarch64__)
    for (size_t i = 0; i < loaded; i++) macho_object_free(&objs[i]);
#elif defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
    for (size_t i = 0; i < loaded; i++) elf_object_free(&objs[i]);
#endif
    for (int i = 0; i < ninputs; i++) free(blobs[i]);
    free(objs);
    free(blobs);
    return result;
}
#endif

static void usage(FILE *out) {
    fputs("tiq " TIQ_VERSION " - Tiq bootstrap compiler\n\n", out);
    fputs("usage:\n", out);
    fputs("  tiq --version\n", out);
    fputs("  tiq run <file.tiq> [-l lib] [-L dir]\n", out);
    fputs("  tiq build <file.tiq> [-o output] [--backend qbe] [--target <triple>] [-l lib] [-L dir]\n", out);
    fputs("  tiq emit-c [--lib] <file.tiq>\n", out);
    fputs("  tiq emit-header <file.tiq> [-o output]\n", out);
    fputs("  tiq emit-obj <file.s> -o <file.o>\n", out);
    fputs("  tiq link-qbe <obj>... -o <exe>\n", out);
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
        ir_lower(root->stmts, root->count, &module, &diag, argv[2]);
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
    if (argc >= 3 && strcmp(argv[1], "emit-obj") == 0) {
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__) || (defined(__riscv) && __riscv_xlen == 64)))
        // M17.3.1/M17.3.3/M17.4.1: integrated assembler + object writer.
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
        if (!input || !output) { usage(stderr); return 2; }
        return cmd_emit_obj(input, output);
#else
        fprintf(stderr, "tiq: emit-obj is only supported on aarch64/x86_64/riscv64 hosts (Darwin or Linux)\n");
        return 2;
#endif
    }
    if (argc >= 3 && strcmp(argv[1], "link-qbe") == 0) {
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__) || (defined(__riscv) && __riscv_xlen == 64)))
        // M17.3.2/M17.3.3: integrated executable linker.
        return cmd_link_qbe(argc, argv);
#else
        fprintf(stderr, "tiq: link-qbe is only supported on aarch64/x86_64 hosts (Darwin or Linux)\n");
        return 2;
#endif
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
        // M17.4.2: --target wasm32-wasi routes through the in-process wasm backend
        if (target && strcmp(target, "wasm32-wasi") == 0) {
            if (link_count > 0) {
                fprintf(stderr, "tiq: -l/-L link options are not supported for wasm32-wasi\n");
                return 2;
            }
            return build_wasm(input, output, &diag);
        }
        return build_target(input, output, target, link_buf, link_count, &diag);
    }
    usage(stderr);
    return 2;
}
