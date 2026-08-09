// M17.3.2: Mach-O arm64 relocatable-object reader.
//
// Parses the subset of MH_OBJECT files produced by the integrated
// writer (macho_obj.c) and by clang for the QBE runtime:
//   - segments with __text, __cstring, __data, __bss, __common and
//     __compact_unwind sections (the latter is carried through and
//     discarded by the linker)
//   - extern relocations: UNSIGNED, SUBTRACTOR, BRANCH26, PAGE21,
//     PAGEOFF12, GOT_LOAD_PAGE21, GOT_LOAD_PAGEOFF12, POINTER_TO_GOT
//   - symbol table with defined, undefined and __common symbols
// Anything outside that subset fails closed with a diagnostic; all
// offsets are bounds-checked against the input buffer.

#define _POSIX_C_SOURCE 200809L
#include "../include/macho_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MACHO_MAGIC_64 0xFEEDFACFu
#define MACHO_CPU_ARM64 0x0100000Cu
#define MACHO_MH_OBJECT 1u
#define MACHO_LC_SEGMENT_64 0x19u
#define MACHO_LC_SYMTAB 0x2u
#define MACHO_LC_DYSYMTAB 0xBu
#define MACHO_LC_LINKER_OPTIMIZATION_HINT 0x2Eu
#define MACHO_LC_BUILD_VERSION 0x32u
#define MACHO_N_STAB 0xE0u
#define MACHO_N_EXT 0x01u
#define MACHO_N_TYPE 0x0Eu
#define MACHO_N_SECT 0x0Eu

static uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int fail(char *err, size_t errlen, const char *msg) {
    snprintf(err, errlen, "macho_read: %s", msg);
    return 1;
}

// Copy a fixed-width 16-byte Mach-O name field into a NUL-terminated
// 17-byte destination.
static void copy_name(const uint8_t *src, char *dst) {
    memcpy(dst, src, 16);
    dst[16] = '\0';
}

int macho_read(const uint8_t *data, size_t len, MachOObject *out,
               char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    if (len < 32) return fail(err, errlen, "not a Mach-O object: truncated header");
    if (rd_u32(data) != MACHO_MAGIC_64)
        return fail(err, errlen, "not a Mach-O 64-bit object (bad magic)");
    if (rd_u32(data + 4) != MACHO_CPU_ARM64)
        return fail(err, errlen, "not an arm64 Mach-O object");
    if (rd_u32(data + 12) != MACHO_MH_OBJECT)
        return fail(err, errlen, "not a relocatable Mach-O object (filetype)");

    uint32_t ncmds = rd_u32(data + 16);
    uint32_t sizeofcmds = rd_u32(data + 20);
    if (32 + (size_t)sizeofcmds > len)
        return fail(err, errlen, "load commands extend past end of file");

    // First pass: count sections and locate LC_SYMTAB.
    uint32_t nsection = 0;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    int have_symtab = 0;
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > 32 + (size_t)sizeofcmds)
            return fail(err, errlen, "truncated load command");
        uint32_t cmd = rd_u32(data + off);
        uint32_t cmdsize = rd_u32(data + off + 4);
        if (cmdsize < 8 || off + cmdsize > 32 + (size_t)sizeofcmds)
            return fail(err, errlen, "load command size out of bounds");
        if (cmd == MACHO_LC_SEGMENT_64) {
            if (cmdsize < 72)
                return fail(err, errlen, "truncated segment command");
            uint32_t nsects = rd_u32(data + off + 64);
            if (72 + (size_t)nsects * 80 != cmdsize)
                return fail(err, errlen, "segment command size mismatch");
            nsection += nsects;
        } else if (cmd == MACHO_LC_SYMTAB) {
            if (cmdsize != 24 || have_symtab)
                return fail(err, errlen, "unsupported LC_SYMTAB shape");
            symoff = rd_u32(data + off + 8);
            nsyms = rd_u32(data + off + 12);
            stroff = rd_u32(data + off + 16);
            strsize = rd_u32(data + off + 20);
            have_symtab = 1;
        } else if (cmd == MACHO_LC_DYSYMTAB ||
                   cmd == MACHO_LC_LINKER_OPTIMIZATION_HINT ||
                   cmd == MACHO_LC_BUILD_VERSION) {
            // Metadata only; the integrated linker re-derives everything
            // it needs, so these are ignored.
        } else {
            return fail(err, errlen, "unsupported load command in object");
        }
        off += cmdsize;
    }
    if (!have_symtab)
        return fail(err, errlen, "object has no symbol table");
    if ((size_t)symoff + (size_t)nsyms * 16 > len)
        return fail(err, errlen, "symbol table extends past end of file");
    if ((size_t)stroff + strsize > len)
        return fail(err, errlen, "string table extends past end of file");

    out->sections = calloc(nsection ? nsection : 1, sizeof(MachOSection));
    out->symbols = calloc(nsyms ? nsyms : 1, sizeof(MachOSymbol));
    if (!out->sections || !out->symbols)
        return fail(err, errlen, "out of memory");
    out->nsection = nsection;
    out->nsymbol = nsyms;

    // Second pass: materialize sections (content + relocations).
    uint32_t si = 0;
    off = 32;
    for (uint32_t i = 0; i < ncmds && si < nsection; i++) {
        uint32_t cmd = rd_u32(data + off);
        uint32_t cmdsize = rd_u32(data + off + 4);
        if (cmd != MACHO_LC_SEGMENT_64) {
            off += cmdsize;
            continue;
        }
        uint32_t nsects = rd_u32(data + off + 64);
        for (uint32_t k = 0; k < nsects; k++, si++) {
            const uint8_t *s = data + off + 72 + (size_t)k * 80;
            MachOSection *sec = &out->sections[si];
            copy_name(s, sec->sectname);
            copy_name(s + 16, sec->segname);
            sec->addr = rd_u64(s + 32);
            sec->size = rd_u64(s + 40);
            uint32_t fileoff = rd_u32(s + 48);
            uint32_t reloff = rd_u32(s + 56);
            uint32_t nreloc = rd_u32(s + 60);
            sec->flags = rd_u32(s + 64);
            int zerofill = (sec->flags & 0xFu) == 0x1u; // S_ZEROFILL
            if (!zerofill) {
                if ((size_t)fileoff + sec->size > len)
                    return fail(err, errlen, "section content past end of file");
                sec->data = malloc(sec->size ? sec->size : 1);
                if (!sec->data) return fail(err, errlen, "out of memory");
                memcpy(sec->data, data + fileoff, sec->size);
            }
            // __compact_unwind is discarded by the linker; its local
            // (non-extern) relocations are never applied, so skip them.
            if (strcmp(sec->sectname, "__compact_unwind") == 0) nreloc = 0;
            if (nreloc > 0) {
                if ((size_t)reloff + (size_t)nreloc * 8 > len)
                    return fail(err, errlen, "relocations past end of file");
                sec->relocs = calloc(nreloc, sizeof(MachOReloc));
                if (!sec->relocs) return fail(err, errlen, "out of memory");
                sec->nreloc = nreloc;
                for (uint32_t r = 0; r < nreloc; r++) {
                    const uint8_t *re = data + reloff + (size_t)r * 8;
                    uint32_t word0 = rd_u32(re);
                    uint32_t word1 = rd_u32(re + 4);
                    if (word0 & 0x80000000u)
                        return fail(err, errlen, "scattered relocations are unsupported");
                    if (!(word1 & (1u << 27)))
                        return fail(err, errlen, "non-extern relocations are unsupported");
                    MachOReloc *mr = &sec->relocs[r];
                    mr->address = word0;
                    mr->sym = (int32_t)(word1 & 0x00FFFFFFu);
                    mr->pcrel = (uint8_t)((word1 >> 24) & 1u);
                    mr->length = (uint8_t)((word1 >> 25) & 3u);
                    mr->type = (uint8_t)((word1 >> 28) & 0xFu);
                    if (mr->sym >= (int32_t)nsyms)
                        return fail(err, errlen, "relocation symbol index out of range");
                    switch (mr->type) {
                    case MACHO_RELOC_UNSIGNED:
                    case MACHO_RELOC_SUBTRACTOR:
                    case MACHO_RELOC_BRANCH26:
                    case MACHO_RELOC_PAGE21:
                    case MACHO_RELOC_PAGEOFF12:
                    case MACHO_RELOC_GOT_LOAD_PAGE21:
                    case MACHO_RELOC_GOT_LOAD_PAGEOFF12:
                    case MACHO_RELOC_POINTER_TO_GOT:
                        break;
                    default:
                        return fail(err, errlen, "unsupported relocation type");
                    }
                }
            }
        }
        off += cmdsize;
    }

    // Symbol table. Section ordinals are 1-based across all segments in
    // load-command order, matching the section array built above.
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *n = data + symoff + (size_t)i * 16;
        uint32_t n_strx = rd_u32(n);
        uint8_t n_type = n[4];
        uint8_t n_sect = n[5];
        uint64_t n_value = rd_u64(n + 8);
        MachOSymbol *sym = &out->symbols[i];
        if (n_type & MACHO_N_STAB)
            return fail(err, errlen, "debug (stab) symbols are unsupported");
        if (n_strx >= strsize)
            return fail(err, errlen, "symbol name out of string-table bounds");
        const char *name = (const char *)data + stroff + n_strx;
        size_t nlen = strnlen(name, strsize - n_strx);
        sym->name = malloc(nlen + 1);
        if (!sym->name) return fail(err, errlen, "out of memory");
        memcpy(sym->name, name, nlen + 1);
        sym->global = (n_type & MACHO_N_EXT) != 0;
        sym->value = n_value;
        if ((n_type & MACHO_N_TYPE) == MACHO_N_SECT) {
            if (n_sect == 0 || n_sect > nsection)
                return fail(err, errlen, "symbol section ordinal out of range");
            sym->section = (int32_t)n_sect - 1;
            sym->common = 0;
        } else if ((n_type & ~MACHO_N_EXT) == 0) {
            // NO_SECT: undefined (value 0) or __common (value = size).
            sym->section = -1;
            sym->common = sym->global && n_value > 0;
        } else {
            return fail(err, errlen, "unsupported symbol type");
        }
    }
    return 0;
}

void macho_object_free(MachOObject *o) {
    if (!o) return;
    for (uint32_t i = 0; i < o->nsection; i++) {
        free(o->sections[i].data);
        free(o->sections[i].relocs);
    }
    free(o->sections);
    for (uint32_t i = 0; i < o->nsymbol; i++) free(o->symbols[i].name);
    free(o->symbols);
    memset(o, 0, sizeof(*o));
}
