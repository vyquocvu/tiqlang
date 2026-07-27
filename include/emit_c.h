#ifndef TIQ_EMIT_C_H
#define TIQ_EMIT_C_H

#include <stdio.h>
#include "diag.h"

// Compile Tiq source to C11 on `out`. Reports problems through `diag`;
// on any diagnostic no (partial) program should be used.
void compile_to_c(const char *source_path, const char *source, FILE *out, DiagContext *diag);

#endif
