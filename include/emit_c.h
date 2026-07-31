#ifndef TIQ_EMIT_C_H
#define TIQ_EMIT_C_H

#include <stdio.h>
#include "diag.h"
#include "semantic.h"

// Compile Tiq source to C11 on `out`. Reports problems through `diag`;
// on any diagnostic no (partial) program should be used.
void compile_to_c(const char *source_path, const char *source, FILE *out, DiagContext *diag);

// M13.1-P6: compile an already-parsed multi-module program (dependency
// post-order, imports stripped) into a single C11 translation unit.
// `root_path` labels backend diagnostics; it never enters the emitted C.
void compile_modules_to_c(SemanticModule *mods, int mod_count, const char *root_path,
                          FILE *out, DiagContext *diag);

#endif
