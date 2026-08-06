#ifndef TIQ_ASM_ARM64_H
#define TIQ_ASM_ARM64_H

#include "asm.h"

// M17.3.1: integrated arm64 assembler + Mach-O object writer.
// M17.3.3: extended with ELF output mode for aarch64 Linux.
//
// asm_arm64.c assembles the exact assembly subset QBE emits for the
// arm64 target (see third_party/qbe/arm64/emit.c and gas.c in
// Gasmacho or Gaself mode). Anything outside that subset fails closed
// with a located diagnostic instead of producing a partial object.
// macho_obj.c serializes the assembled unit as a minimal arm64 Mach-O
// relocatable object (MH_OBJECT) accepted by the host linker.
// elf_obj.c serializes as an ELF64 aarch64 relocatable object.

// Assemble the QBE arm64 Mach-O assembly subset. Returns 0 on success.
// On failure returns 1 and fills u->errmsg / u->errline; no section
// contents are meaningful after a failure.
int asm_arm64_assemble(AsmUnit *u, const char *text, size_t len);

// Write the unit as a Mach-O arm64 relocatable object. Returns 0 on
// success; non-zero on write failure. Requires a successfully assembled
// unit with fmt == ASM_FMT_MACHO.
int macho_obj_write(const AsmUnit *u, FILE *out);

// Write the unit as an ELF64 aarch64 relocatable object. Returns 0 on
// success; non-zero on write failure. Requires a successfully assembled
// unit with fmt == ASM_FMT_ELF.
int elf_obj_write(const AsmUnit *u, FILE *out);

#endif
