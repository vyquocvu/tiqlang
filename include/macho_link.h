#ifndef TIQ_MACHO_LINK_H
#define TIQ_MACHO_LINK_H

#include <stddef.h>
#include <stdint.h>

// M17.3.2: integrated Mach-O arm64 executable linker.
//
// macho_read parses the subset of Mach-O relocatable objects produced
// by the integrated writer (macho_obj.c) and by clang for the QBE
// runtime (see build/runtime_qbe.o): anonymous/standard segments with
// __text, __cstring, __data, __bss, __common sections; extern
// relocations of the ARM64_RELOC_* types listed below. Anything else
// fails closed with a diagnostic in the caller-provided buffer.
//
// link_macho_exec combines parsed objects into a runnable MH_EXECUTE
// with self-generated ad-hoc code signature. Undefined symbols are
// resolved against the objects first, then a fixed libSystem export
// set bound via dyld stubs; anything unresolved fails closed naming
// the symbol. Output is deterministic: identical inputs produce
// byte-identical executables.

enum {
    // Relocation types as encoded in mach-o/arm64/reloc.h.
    MACHO_RELOC_UNSIGNED = 0,
    MACHO_RELOC_SUBTRACTOR = 1,
    MACHO_RELOC_BRANCH26 = 2,
    MACHO_RELOC_PAGE21 = 3,
    MACHO_RELOC_PAGEOFF12 = 4,
    MACHO_RELOC_GOT_LOAD_PAGE21 = 5,
    MACHO_RELOC_GOT_LOAD_PAGEOFF12 = 6,
    MACHO_RELOC_POINTER_TO_GOT = 7
};

typedef struct {
    uint32_t address;   // section-relative offset of the relocated field
    int32_t sym;        // symbol index in the containing object
    uint8_t pcrel;
    uint8_t length;     // 2 = 4 bytes, 3 = 8 bytes
    uint8_t type;       // MACHO_RELOC_*
} MachOReloc;

typedef struct {
    char sectname[17];
    char segname[17];
    uint32_t flags;     // section_64 flags (S_ZEROFILL etc.)
    uint64_t addr;      // running section address (relocatable objects use
                        // running addresses; symbol values include it)
    uint8_t *data;      // owned copy of section content (NULL for zerofill)
    uint64_t size;      // section size (zerofill: allocation size)
    MachOReloc *relocs;
    uint32_t nreloc;
} MachOSection;

typedef struct {
    char *name;         // owned, exactly as spelled in the object
    int32_t section;    // index into sections, -1 when undefined/common
    uint64_t value;     // section-relative offset, or size for common
    uint8_t global;
    uint8_t common;     // __common symbol (bss-like, value = size)
} MachOSymbol;

typedef struct {
    MachOSection *sections;
    uint32_t nsection;
    MachOSymbol *symbols;
    uint32_t nsymbol;
} MachOObject;

// Parse a Mach-O arm64 relocatable object. Returns 0 on success.
// On failure returns 1 and writes a NUL-terminated diagnostic to err.
int macho_read(const uint8_t *data, size_t len, MachOObject *out,
               char *err, size_t errlen);
void macho_object_free(MachOObject *o);

// Link parsed objects into a runnable arm64 Mach-O executable written
// to *out (malloc'd, caller frees). entry is the mangled entry symbol
// ("_main"). Returns 0 on success; on failure returns 1 with a
// diagnostic in err and *out unchanged.
int link_macho_exec(const MachOObject *objs, size_t nobj, const char *entry,
                    uint8_t **out, size_t *out_len, char *err, size_t errlen);

#endif
