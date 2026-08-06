#ifndef TIQ_ASM_RV64_H
#define TIQ_ASM_RV64_H

#include "asm.h"

// M17.4.1: integrated riscv64 assembler + ELF object writer/linker.
//
// asm_rv64.c assembles the exact assembly subset QBE emits for the
// rv64 target in Gaself mode (see third_party/qbe/rv64/emit.c and
// gas.c). RISC-V pseudo-instructions exercised by QBE (`la`, `call`,
// symbol-relative loads/stores, `li`, `mv`, `sext.*/zext.*`, branch
// aliases) are expanded by the assembler into explicit instruction
// sequences carrying paired PC-relative relocations. Anything outside
// the subset fails closed with a located diagnostic; partial objects
// are never produced.
//
// elf_obj.c serializes the assembled unit as an ELF64 riscv64
// relocatable object; link_elf_exec (src/elf_link.c) resolves the
// static, program-internal relocations to produce an executable.

// Assemble the QBE riscv64 (Gaself) assembly subset. Returns 0 on
// success. On failure returns 1 and fills u->errmsg / u->errline. The
// caller must set u->fmt = ASM_FMT_ELF before assembling.
int asm_rv64_assemble(AsmUnit *u, const char *text, size_t len);

#endif