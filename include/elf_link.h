// M17.3.4: integrated ELF64 executable linker (aarch64, x86_64, riscv64).
//
// elf_read parses the subset of ELF64 relocatable objects produced by
// the integrated writer (elf_obj.c) and by cc for the QBE runtime
// (build/runtime_qbe.o): .text, .data, .bss, .rodata, .cstring
// sections; R_AARCH64_*, R_X86_64_*, and R_RISCV_* relocations.
// Anything else fails closed with a diagnostic in the caller-provided
// buffer.
//
// link_elf_exec combines parsed objects into a runnable ELF executable
// with dynamic linking to libc. Undefined symbols are resolved against
// the objects first, then a fixed libc export set bound via GOT/PLT
// stubs; anything unresolved fails closed naming the symbol. Output is
// deterministic: identical inputs produce byte-identical executables.
#ifndef TIQ_ELF_LINK_H
#define TIQ_ELF_LINK_H

#include <stddef.h>
#include <stdint.h>

// ELF64 constants
#define ELF_MAGIC "\x7f" "ELF"
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ELFOSABI_NONE 0
#define ET_REL 1
#define ET_EXEC 2
#define EM_AARCH64 183
#define EM_X86_64 62
#define EM_RISCV 243

// Section types
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_DYNSYM 11
#define SHT_DYNAMIC 6

// Section flags
#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

// Symbol binding
#define STB_LOCAL 0
#define STB_GLOBAL 1

// Symbol types
#define STT_NOTYPE 0
#define STT_FUNC 2
#define STT_SECTION 3

// aarch64 relocation types
enum {
    R_AARCH64_NONE               = 0,
    R_AARCH64_ABS64              = 257,
    R_AARCH64_ADR_PREL_PG_HI21   = 275,
    R_AARCH64_ADD_ABS_LO12_NC    = 277,
    R_AARCH64_CALL26             = 283,
    R_AARCH64_ADR_GOT_PAGE       = 308,
    R_AARCH64_LD64_GOT_LO12_NC   = 309,
    R_AARCH64_JUMP_SLOT          = 1026,
};

// x86_64 relocation types
enum {
    R_X86_64_NONE      = 0,
    R_X86_64_64        = 1,
    R_X86_64_PC32      = 2,
    R_X86_64_PLT32     = 4,
    R_X86_64_GOTPCRELX = 42,
};

// riscv64 relocation types (M17.4.1).
// Internal ASM_RELOC_RISCV_* values (see asm.h) are translated to these
// by a switch table in the object writer/reader and linker.
enum {
    R_RISCV_NONE         = 0,
    R_RISCV_32           = 1,
    R_RISCV_64           = 2,
    R_RISCV_RELATIVE     = 3,
    R_RISCV_JUMP_SLOT    = 5,
    R_RISCV_GLOB_DAT     = 6,
    R_RISCV_BRANCH       = 16,
    R_RISCV_JAL          = 17,
    R_RISCV_PCREL_HI20   = 23,
    R_RISCV_PCREL_LO12_I = 24,
    R_RISCV_PCREL_LO12_S = 25,
    R_RISCV_HI20         = 26,
    R_RISCV_LO12_I       = 27,
    R_RISCV_LO12_S       = 28,
};

// Program header types
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_GNU_STACK 0x6474e551

// Dynamic tags
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTGOT 3
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 10
#define DT_RELAENT 12
#define DT_STRSZ 11
#define DT_SYMENT 13
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define DT_PLTREL 20
#define DT_FLAGS 30
#define DT_FLAGS_1 0x6ffffffb

// Dynamic flags
#define DF_BIND_NOW 0x8
#define DF_1_NOW 0x1

typedef struct {
    uint64_t offset;   // section-relative offset of the relocated field
    int32_t sym;       // symbol index in the containing object
    int32_t type;      // R_AARCH64_* (internal representation)
    int64_t addend;
} ElfReloc;

typedef struct {
    char name[17];     // section name (NUL-terminated, up to 16 chars)
    uint32_t type;     // SHT_*
    uint64_t flags;    // SHF_*
    uint64_t addr;     // running section address
    uint8_t *data;     // owned copy of section content (NULL for NOBITS)
    uint64_t size;     // section size
    ElfReloc *relocs;
    uint32_t nreloc;
    uint32_t link;     // associated section (e.g. symtab -> strtab)
    uint64_t entsize;  // entry size for fixed-entry sections
} ElfSection;

typedef struct {
    char *name;        // owned, exactly as spelled in the object
    int32_t section;   // index into sections, -1 when undefined
    uint64_t value;    // section-relative offset, or 0 for common
    uint8_t binding;   // STB_*
    uint8_t type;      // STT_*
    uint8_t common;    // SHN_COMMON symbol (bss-like, value = size)
} ElfSymbol;

typedef struct {
    ElfSection *sections;
    uint32_t nsection;
    ElfSymbol *symbols;
    uint32_t nsymbol;
    uint16_t machine;  // EM_AARCH64, EM_X86_64, or EM_RISCV
} ElfObject;

// Parse an ELF64 relocatable object (aarch64, x86_64, or riscv64).
// Returns 0 on success; on failure returns 1 and writes a NUL-terminated
// diagnostic to err.
int elf_read(const uint8_t *data, size_t len, ElfObject *out,
             char *err, size_t errlen);
void elf_object_free(ElfObject *o);

// Link parsed objects into a runnable ELF executable written
// to *out (malloc'd, caller frees). entry is the entry symbol name
// ("main"). Returns 0 on success; on failure returns 1 with a
// diagnostic in err and *out unchanged.
int link_elf_exec(const ElfObject *objs, size_t nobj, const char *entry,
                  uint8_t **out, size_t *out_len, char *err, size_t errlen);

#endif
