#ifndef TIQ_ASM_ARM64_H
#define TIQ_ASM_ARM64_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

enum {
    ASM_SEC_TEXT = 0,
    ASM_SEC_DATA = 1,
    ASM_SEC_BSS = 2,
    ASM_SEC_RODATA = 3,
    ASM_SEC_COUNT = 4
};

// Output format: controls relocation encoding and object writer choice.
enum { ASM_FMT_MACHO = 0, ASM_FMT_ELF = 1 };

// Internal relocation type constants (format-agnostic).
// The object writer translates these to Mach-O or ELF encodings.
enum {
    ASM_RELOC_UNSIGNED = 0,
    ASM_RELOC_BRANCH26 = 2,
    ASM_RELOC_PAGE21 = 3,
    ASM_RELOC_PAGEOFF12 = 4
};

typedef struct {
    int32_t address;   // section-relative offset of the relocated field
    int32_t symbol;    // index into the unit symbol table
    unsigned pcrel;    // 1 for pc-relative fixups (branches, adrp)
    unsigned length;   // 2 = 4 bytes, 3 = 8 bytes
    unsigned type;     // MACHO_ARM64_RELOC_*
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
    int fmt;           // ASM_FMT_MACHO or ASM_FMT_ELF
    int has_gnu_stack; // .note.GNU-stack directive seen (ELF only)
} AsmUnit;

void asm_unit_init(AsmUnit *u);
void asm_unit_free(AsmUnit *u);

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
