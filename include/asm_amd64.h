#ifndef TIQ_ASM_AMD64_H
#define TIQ_ASM_AMD64_H

#include "asm.h"

// M17.3.4: integrated x86_64 assembler for the QBE amd64 assembly subset.
//
// asm_amd64.c assembles the exact assembly subset QBE emits for the
// amd64 target (see third_party/qbe/amd64/emit.c and gas.c in Gaself
// mode). Anything outside that subset fails closed with a located
// diagnostic instead of producing a partial object.
//
// The assembler is platform-independent: it parses AT&T-syntax x86_64
// assembly and produces section bytes + relocations. The output can be
// serialized as PE/COFF (pe_obj.c) or ELF (elf_obj.c with x86_64
// extensions).

// Assemble the QBE amd64 assembly subset. Returns 0 on success.
// On failure returns 1 and fills u->errmsg / u->errline; no section
// contents are meaningful after a failure.
int asm_amd64_assemble(AsmUnit *u, const char *text, size_t len);

// Write the unit as a PE/COFF x86_64 relocatable object. Returns 0 on
// success; non-zero on write failure.
int pe_obj_write(const AsmUnit *u, FILE *out);

#endif
