#ifndef TIQ_PE_LINK_H
#define TIQ_PE_LINK_H

#include "asm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// M17.3.4: PE/COFF types and API for the x86_64 Windows object writer
// and executable linker.

// PE/COFF constants.
#define IMAGE_FILE_MACHINE_AMD64 0x8664u

// PE/COFF relocation types for AMD64.
#define IMAGE_REL_AMD64_ADDR64    0x0001u  // absolute 64-bit
#define IMAGE_REL_AMD64_ADDR32NB  0x0003u  // 32-bit address without base
#define IMAGE_REL_AMD64_REL32     0x0004u  // 32-bit PC-relative
#define IMAGE_REL_AMD64_REL32_5   0x0008u  // 32-bit PC-relative + 5

// Parsed PE relocatable object (mirrors ElfObject/MachOObject).
typedef struct {
    int32_t address;
    int32_t symbol;
    unsigned pcrel;
    unsigned length;
    unsigned type;   // internal ASM_RELOC_* type
} PeReloc;

typedef struct {
    char *name;
    int section;     // 0-based section index, -1 for undefined
    int64_t value;
    int global;
    int storage_class; // COFF storage class
} PeSymbol;

typedef struct {
    char name[16];
    uint8_t *data;
    size_t data_len;
    PeReloc *relocs;
    size_t nreloc;
    uint32_t characteristics;
} PeSection;

typedef struct {
    PeSection *sections;
    size_t nsection;
    PeSymbol *symbols;
    size_t nsymbol;
    uint16_t machine;
} PeObject;

// Read a PE/COFF relocatable object from memory. Returns 0 on success.
int pe_read(const uint8_t *data, size_t size, PeObject *obj);

// Free a parsed PE object.
void pe_object_free(PeObject *obj);

// Link parsed PE objects into a PE32+ executable. Returns 0 on success.
int link_pe_exec(const PeObject *objs, int nobj, const char *entry,
                 const char **ext_syms, int next_syms, FILE *out);

#endif
