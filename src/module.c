// M13.1-P6: module loader (LANGUAGE_SPEC §17.6). DFS from the root file,
// import declaration order; modules are deduplicated by canonical
// filesystem path (the same file imported via different relative
// spellings loads once) and appended in post-order, so imported files
// always precede their importers in the emitted translation unit.
// Canonical paths are dedupe keys only: diagnostics print the path as
// written/joined, and nothing filesystem-derived enters generated C.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/module.h"

// Hard bound on the in-progress DFS stack; true cycles are detected
// exactly (E28), so overflowing this means a pathologically deep chain,
// which fails closed rather than recursing without bound.
#define TIQ_MAX_IMPORT_DEPTH 64

typedef struct {
    Program *prog;
    DiagContext *diag;
    // In-progress stack for cycle detection; both strings are borrowed
    // from the recursion frames that own them.
    struct {
        const char *canon;
        const char *path;
    } stack[TIQ_MAX_IMPORT_DEPTH];
    int depth;
} Loader;

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "tiq: out of memory\n"); exit(1); }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1U;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

// Read a whole file; NULL on any I/O failure (caller reports).
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    data = xmalloc((size_t)size + 1U);
    if (fread(data, 1U, (size_t)size, file) != (size_t)size) {
        free(data); fclose(file); return NULL;
    }
    data[size] = '\0';
    fclose(file);
    return data;
}

// Join an import path onto the importing file's directory (POSIX '/').
// Absolute import paths pass through unchanged.
static char *join_path(const char *importer, const char *rel) {
    const char *slash = strrchr(importer, '/');
    size_t dir_len, rel_len = strlen(rel);
    char *joined;
    if (rel[0] == '/') return xstrdup(rel);
    dir_len = slash ? (size_t)(slash - importer) + 1U : 0U;
    joined = xmalloc(dir_len + rel_len + 1U);
    memcpy(joined, importer, dir_len);
    memcpy(joined + dir_len, rel, rel_len + 1U);
    return joined;
}

// The path operand as written: the string token's contents between the
// quotes, verbatim (escape sequences are not interpreted; §17.6).
static char *import_path_text(Token path) {
    size_t len = path.length >= 2U ? path.length - 2U : 0U;
    char *text = xmalloc(len + 1U);
    memcpy(text, path.start + 1, len);
    text[len] = '\0';
    return text;
}

static void append_module(Program *prog, Module *m) {
    if (prog->count + 1 > prog->capacity) {
        int new_cap = prog->capacity < 8 ? 8 : prog->capacity * 2;
        Module *grown = realloc(prog->mods, sizeof(Module) * (size_t)new_cap);
        if (!grown) { fprintf(stderr, "tiq: out of memory\n"); exit(1); }
        prog->mods = grown;
        prog->capacity = new_cap;
    }
    prog->mods[prog->count++] = *m;
}

// Bounded append for the cycle-chain message; truncation keeps the
// diagnostic well-formed on absurd chains.
static void chain_append(char *buf, size_t cap, const char *part) {
    size_t used = strlen(buf);
    if (used + 1U < cap) snprintf(buf + used, cap - used, "%s", part);
}

// Load `fs_path` (already joined onto its importer's directory) and,
// depth-first, everything it imports. `importer_path`/`import_line`
// locate the responsible import declaration; both are NULL/0 for the
// root file. `written` is the operand as the user wrote it.
static bool load_module(Loader *ld, const char *fs_path, const char *written,
                        const char *importer_path, int import_line) {
    DiagContext *diag = ld->diag;
    char *canon = realpath(fs_path, NULL);
    const char *open_path = fs_path;

    // M15: cwd fallback — if the relative join fails, retry from the
    // current working directory. This allows `import "std/json.tiq"`
    // to resolve from any file depth when tiq is invoked from the
    // project root.
    char *alt_path = NULL;
    if (!canon && importer_path) {
        char cwd_buf[4096];
        if (getcwd(cwd_buf, sizeof cwd_buf)) {
            size_t cl = strlen(cwd_buf);
            size_t wl = strlen(written);
            alt_path = xmalloc(cl + 1U + wl + 1U);
            memcpy(alt_path, cwd_buf, cl);
            alt_path[cl] = '/';
            memcpy(alt_path + cl + 1U, written, wl + 1U);
            canon = realpath(alt_path, NULL);
            if (canon) open_path = alt_path;
        }
    }

    if (!canon) {
        free(alt_path);
        if (importer_path) {
            char msg[512];
            snprintf(msg, sizeof msg, "module not found: \"%s\"", written);
            diag_error(diag, importer_path, import_line, ERR_MODULE_NOT_FOUND, msg);
        } else {
            fprintf(stderr, "tiq: cannot open %s: %s\n", fs_path, strerror(errno));
            diag->has_error = true;
        }
        return false;
    }

    // Cycle: the file is still being loaded further up this DFS chain.
    for (int i = 0; i < ld->depth; i++) {
        if (strcmp(ld->stack[i].canon, canon) == 0) {
            char msg[1024] = "circular import: ";
            for (int j = i; j < ld->depth; j++) {
                chain_append(msg, sizeof msg, ld->stack[j].path);
                chain_append(msg, sizeof msg, " -> ");
            }
            chain_append(msg, sizeof msg, ld->stack[i].path);
            diag_error(diag, importer_path ? importer_path : fs_path,
                       import_line, ERR_CIRCULAR_IMPORT, msg);
            free(canon);
            return false;
        }
    }

    // Dedupe: already loaded via some other spelling — skip silently.
    for (int i = 0; i < ld->prog->count; i++) {
        if (strcmp(ld->prog->mods[i].canon, canon) == 0) {
            free(canon);
            return true;
        }
    }

    if (ld->depth >= TIQ_MAX_IMPORT_DEPTH) {
        diag_error(diag, importer_path ? importer_path : fs_path, import_line,
                   ERR_CIRCULAR_IMPORT, "import nesting exceeds 64 levels");
        free(canon);
        return false;
    }

    char *source = read_file(open_path);
    if (!source) {
        free(alt_path);
        if (importer_path) {
            char msg[512];
            snprintf(msg, sizeof msg, "module not found: \"%s\"", written);
            diag_error(diag, importer_path, import_line, ERR_MODULE_NOT_FOUND, msg);
        } else {
            fprintf(stderr, "tiq: cannot open %s: %s\n", fs_path, strerror(errno));
            diag->has_error = true;
        }
        free(canon);
        return false;
    }

    Module m;
    memset(&m, 0, sizeof m);
    m.canon = canon;
    m.path = xstrdup(open_path);
    m.source = source;
    free(alt_path);
    parser_init(&m.parser, m.source, m.path, diag);
    m.stmts = parser_parse(&m.parser, &m.count);
    if (diag->has_error) {
        // Parse error (including imports after a non-import item): fail
        // closed; keep the module so program_free releases it.
        append_module(ld->prog, &m);
        return false;
    }

    // The parser guarantees imports form a prefix on success.
    m.import_count = 0;
    while (m.import_count < m.count && m.stmts[m.import_count] &&
           m.stmts[m.import_count]->kind == AST_IMPORT)
        m.import_count++;

    ld->stack[ld->depth].canon = canon;
    ld->stack[ld->depth].path = m.path;
    ld->depth++;
    for (int i = 0; i < m.import_count; i++) {
        AstNode *imp = m.stmts[i];
        if (imp->as.import_stmt.path.kind != TOK_STRING) continue;
        char *rel = import_path_text(imp->as.import_stmt.path);
        char *joined = join_path(m.path, rel);
        bool ok = load_module(ld, joined, rel, m.path, imp->token.line);
        free(joined);
        free(rel);
        if (!ok) {
            ld->depth--;
            append_module(ld->prog, &m);
            return false;
        }
    }
    ld->depth--;

    // Post-order: every import of this module is already appended.
    append_module(ld->prog, &m);
    return true;
}

bool program_load(Program *prog, const char *root_path, DiagContext *diag) {
    Loader ld;
    memset(prog, 0, sizeof *prog);
    memset(&ld, 0, sizeof ld);
    ld.prog = prog;
    ld.diag = diag;

    bool ok = load_module(&ld, root_path, root_path, NULL, 0);
    if (!ok || diag->has_error) return false;

    prog->sem = xmalloc(sizeof(SemanticModule) *
                        (size_t)(prog->count > 0 ? prog->count : 1));
    for (int i = 0; i < prog->count; i++) {
        Module *m = &prog->mods[i];
        prog->sem[i].stmts = m->stmts + m->import_count;
        prog->sem[i].count = m->count - m->import_count;
        prog->sem[i].path = m->path;
    }
    return true;
}

void program_free(Program *prog) {
    for (int i = 0; i < prog->count; i++) {
        Module *m = &prog->mods[i];
        parser_free(&m->parser);
        free(m->source);
        free(m->canon);
        free(m->path);
    }
    free(prog->mods);
    free(prog->sem);
    prog->mods = NULL;
    prog->sem = NULL;
    prog->count = prog->capacity = 0;
}
