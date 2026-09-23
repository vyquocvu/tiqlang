// Native amd64 code emitter — translates Tiq SSA IR directly to amd64
// assembly text, bypassing the external QBE binary.
//
// Pipeline: IrModule -> amd64 assembly text -> integrated assembler -> .o
//           -> integrated linker -> executable
//
// Register allocation: stack-spill. Every SSA register gets a dedicated
// stack slot. R10/R11 serve as scratch for computations. PHI values use
// per-block temp slots with two-phase parallel copy.
#ifndef TIQ_EMIT_NATIVE_H
#define TIQ_EMIT_NATIVE_H

#include <stdio.h>
#include <stdbool.h>
#include "ir.h"

// Emit native amd64 assembly for the given IR module to `out`.
// The output is a complete assembly file that the integrated assembler
// (asm_amd64.c) can process. External runtime symbols (tiq_print_i64,
// tiq_print_str, etc.) are referenced but not defined.
// Returns true on success, false on error.
bool emit_native(FILE *out, const IrModule *module);

#endif
