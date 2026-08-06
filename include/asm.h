#ifndef TIQ_ASM_H
#define TIQ_ASM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// M17.3.4: shared assembler types and constants.
//
// These types are used by all integrated assemblers (asm_arm64.c for
// aarch64, asm_amd64.c for x86_64) and object writers (macho_obj.c,
// elf_obj.c, pe_obj.c). Each assembler parses its architecture's QBE
// assembly subset and produces an AsmUnit with section contents,
// symbols, and relocations. Each object writer serializes the AsmUnit
// in the appropriate format (Mach-O, ELF, PE/COFF).

enum {
    ASM_SEC_TEXT = 0,
    ASM_SEC_DATA = 1,
    ASM_SEC_BSS = 2,
    ASM_SEC_RODATA = 3,
    ASM_SEC_COUNT = 4
};

// Output format: controls relocation encoding and object writer choice.
enum { ASM_FMT_MACHO = 0, ASM_FMT_ELF = 1, ASM_FMT_PE = 2 };

// Internal relocation type constants (format-agnostic).
// The object writer translates these to Mach-O, ELF, or PE encodings.
//
// arm64 relocations (M17.3.1/M17.3.3):
enum {
    ASM_RELOC_UNSIGNED = 0,     // absolute (64-bit for arm64/PE, 32-bit for Mach-O)
    ASM_RELOC_BRANCH26 = 2,     // arm64: BL/BRANCH26
    ASM_RELOC_PAGE21 = 3,       // arm64: ADRP/PAGE21
    ASM_RELOC_PAGEOFF12 = 4,    // arm64: ADD/PAGEOFF12
    // x86_64 relocations (M17.3.4):
    ASM_RELOC_X86_64_64 = 10,          // absolute 64-bit (data .quad)
    ASM_RELOC_X86_64_PC32 = 11,        // PC-relative 32-bit (callq, jcc, RIP-relative)
    ASM_RELOC_X86_64_GOTPCRELX = 12    // GOT-relative (movq sym(%rip), %reg)
};

typedef struct {
    int32_t address;   // section-relative offset of the relocated field
    int32_t symbol;    // index into the unit symbol table
    unsigned pcrel;    // 1 for pc-relative fixups (branches, adrp, RIP-relative)
    unsigned length;   // 2 = 4 bytes, 3 = 8 bytes
    unsigned type;     // ASM_RELOC_* internal type
} AsmReloc;

typedef struct {
    char *name;        // owned, NUL-terminated, exactly as spelled in asm
    int section;       // ASM_SEC_* when defined, -1 when undefined
    int64_t value;     // section-relative offset when defined
    int global;        // named by a .globl directive
} AsmSymbol;

typedef struct {
    uint8_t *bytes;
    size_t len, cap;
    AsmReloc *relocs;
    size_t nreloc, reloc_cap;
    int used;          // section appeared in the source
} AsmSectionOut;

typedef struct {
    AsmSectionOut sec[ASM_SEC_COUNT];
    AsmSymbol *syms;
    size_t nsym, sym_cap;
    char errmsg[256];
    int errline;
    int has_error;
    int fmt;           // ASM_FMT_MACHO, ASM_FMT_ELF, or ASM_FMT_PE
    int has_gnu_stack; // .note.GNU-stack directive seen (ELF only)
    uint16_t machine;  // ELF machine: EM_AARCH64 or EM_X86_64
} AsmUnit;

void asm_unit_init(AsmUnit *u);
void asm_unit_free(AsmUnit *u);

#endif
