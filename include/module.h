#ifndef TIQ_MODULE_H
#define TIQ_MODULE_H

#include <stdbool.h>
#include "parser.h"
#include "semantic.h"
#include "diag.h"

// M13.1-P6: module loader (LANGUAGE_SPEC §17.6). A Module owns its source
// buffer, its diagnostic path string, and the parser arena holding its
// AST; tokens reference spans inside `source`, so all three stay alive
// until program_free.
typedef struct {
    char *canon;      // canonical filesystem path — dedupe key only; never
                      // printed and never emitted into generated C
    char *path;       // path used in diagnostics (as given / as joined)
    char *source;     // owned source text
    Parser parser;    // owns every AstNode via its arena
    AstNode **stmts;  // all top-level statements, import prefix included
    int count;
    int import_count; // leading AST_IMPORT statements (always a prefix)
} Module;

// A loaded program: modules in dependency post-order (imported files
// first; DFS by import declaration order, dedupe keeps the first visit).
typedef struct {
    Module *mods;
    int count;
    int capacity;
    // Per-module semantic views (import prefix stripped), parallel to
    // mods; built only on successful load.
    SemanticModule *sem;
} Program;

// DFS-load `root_path` and everything it imports. Returns false when any
// diagnostic was reported (fail closed). The Program must be freed with
// program_free in both outcomes.
bool program_load(Program *prog, const char *root_path, DiagContext *diag);
void program_free(Program *prog);

#endif
