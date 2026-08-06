// M17.3.4: PE/COFF relocatable-object reader for x86_64.
//
// Parses the subset of PE/COFF relocatable objects produced by the
// system compiler (e.g. x86_64-w64-mingw32-gcc -c) for the QBE runtime
// and by pe_obj_write. Anything unsupported fails closed.

#include "../include/pe_link.h"

#include <stdlib.h>
#include <string.h>

// Bounds-checked read helpers.
static int read_u16(const uint8_t *p, size_t end, size_t off, uint16_t *out) {
    if (off + 2 > end) return -1;
    *out = (uint16_t)p[off] | ((uint16_t)p[off + 1] << 8);
    return 0;
}

static int read_u32(const uint8_t *p, size_t end, size_t off, uint32_t *out) {
    if (off + 4 > end) return -1;
    *out = (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) |
           ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
    return 0;
}

// Translate PE/COFF relocation type to internal representation.
static int internal_reloc_type(uint16_t pe_type) {
    switch (pe_type) {
        case 0x0001: return ASM_RELOC_X86_64_64;        // IMAGE_REL_AMD64_ADDR64
        case 0x0004: return ASM_RELOC_X86_64_PC32;       // IMAGE_REL_AMD64_REL32
        case 0x0005: return ASM_RELOC_X86_64_GOTPCRELX;  // IMAGE_REL_AMD64_REL32_1
        case 0x0008: return ASM_RELOC_X86_64_PC32;       // IMAGE_REL_AMD64_REL32_5
        default:     return -1;
    }
}

int pe_read(const uint8_t *data, size_t size, PeObject *obj) {
    memset(obj, 0, sizeof(*obj));
    if (size < 20) return 1;

    // Parse COFF header (20 bytes).
    uint16_t machine, nsec, nopt, chars;
    uint32_t timestamp, symtab_off, nsym;
    if (read_u16(data, size, 0, &machine) != 0) return 1;
    if (read_u16(data, size, 2, &nsec) != 0) return 1;
    if (read_u32(data, size, 4, &timestamp) != 0) return 1;
    if (read_u32(data, size, 8, &symtab_off) != 0) return 1;
    if (read_u32(data, size, 12, &nsym) != 0) return 1;
    if (read_u16(data, size, 16, &nopt) != 0) return 1;
    if (read_u16(data, size, 18, &chars) != 0) return 1;
    (void)timestamp; (void)chars;

    if (machine != IMAGE_FILE_MACHINE_AMD64) return 1;
    obj->machine = machine;

    // Parse section headers (40 bytes each, starting at offset 20).
    size_t sec_hdr_off = 20 + nopt;
    obj->sections = calloc(nsec, sizeof(PeSection));
    if (!obj->sections) return 1;
    obj->nsection = nsec;

    for (uint16_t i = 0; i < nsec; i++) {
        size_t off = sec_hdr_off + (size_t)i * 40;
        if (off + 40 > size) { pe_object_free(obj); return 1; }
        PeSection *sec = &obj->sections[i];
        // Name (8 bytes).
        memcpy(sec->name, data + off, 8);
        sec->name[8] = '\0';
        uint32_t vsize, vaddr, raw_size, raw_ptr, reloc_ptr, linenum_ptr;
        uint16_t nreloc, nlinenum;
        uint32_t sec_chars;
        if (read_u32(data, size, off + 8, &vsize) != 0) { pe_object_free(obj); return 1; }
        if (read_u32(data, size, off + 12, &vaddr) != 0) { pe_object_free(obj); return 1; }
        if (read_u32(data, size, off + 16, &raw_size) != 0) { pe_object_free(obj); return 1; }
        if (read_u32(data, size, off + 20, &raw_ptr) != 0) { pe_object_free(obj); return 1; }
        if (read_u32(data, size, off + 24, &reloc_ptr) != 0) { pe_object_free(obj); return 1; }
        if (read_u32(data, size, off + 28, &linenum_ptr) != 0) { pe_object_free(obj); return 1; }
        if (read_u16(data, size, off + 32, &nreloc) != 0) { pe_object_free(obj); return 1; }
        if (read_u16(data, size, off + 34, &nlinenum) != 0) { pe_object_free(obj); return 1; }
        if (read_u32(data, size, off + 36, &sec_chars) != 0) { pe_object_free(obj); return 1; }
        (void)vsize; (void)vaddr; (void)linenum_ptr; (void)nlinenum;
        sec->characteristics = sec_chars;
        sec->data_len = raw_size;
        if (raw_size > 0 && raw_ptr + raw_size <= size) {
            sec->data = malloc(raw_size);
            if (!sec->data) { pe_object_free(obj); return 1; }
            memcpy(sec->data, data + raw_ptr, raw_size);
        }
        // Parse relocations.
        if (nreloc > 0 && reloc_ptr > 0) {
            sec->relocs = calloc(nreloc, sizeof(PeReloc));
            if (!sec->relocs) { pe_object_free(obj); return 1; }
            sec->nreloc = nreloc;
            for (uint16_t r = 0; r < nreloc; r++) {
                size_t roff = reloc_ptr + (size_t)r * 10;
                if (roff + 10 > size) { pe_object_free(obj); return 1; }
                uint32_t va, sym_idx;
                uint16_t rtype;
                if (read_u32(data, size, roff, &va) != 0) { pe_object_free(obj); return 1; }
                if (read_u32(data, size, roff + 4, &sym_idx) != 0) { pe_object_free(obj); return 1; }
                if (read_u16(data, size, roff + 8, &rtype) != 0) { pe_object_free(obj); return 1; }
                int itype = internal_reloc_type(rtype);
                if (itype < 0) { pe_object_free(obj); return 1; }
                sec->relocs[r].address = (int32_t)va;
                sec->relocs[r].symbol = (int32_t)sym_idx; // will be resolved after symbol table
                sec->relocs[r].pcrel = (itype == ASM_RELOC_X86_64_PC32 || itype == ASM_RELOC_X86_64_GOTPCRELX) ? 1 : 0;
                sec->relocs[r].length = (itype == ASM_RELOC_X86_64_64) ? 3 : 2;
                sec->relocs[r].type = (unsigned)itype;
            }
        }
    }

    // Parse symbol table (18 bytes each).
    if (nsym > 0 && symtab_off > 0) {
        obj->symbols = calloc(nsym, sizeof(PeSymbol));
        if (!obj->symbols) { pe_object_free(obj); return 1; }
        obj->nsymbol = nsym;
        // String table starts right after symbol table.
        size_t strtab_off = symtab_off + (size_t)nsym * 18;
        const uint8_t *strtab = (strtab_off + 4 <= size) ? data + strtab_off : NULL;
        size_t strtab_size = 0;
        if (strtab) {
            uint32_t stsz;
            if (read_u32(data, size, strtab_off, &stsz) == 0) strtab_size = stsz;
        }

        for (uint32_t i = 0; i < nsym; i++) {
            size_t off = symtab_off + (size_t)i * 18;
            if (off + 18 > size) { pe_object_free(obj); return 1; }
            PeSymbol *sym = &obj->symbols[i];
            // Name (8 bytes).
            if (data[off] == '/') {
                // Long name: /offset into string table.
                char num[8];
                int ni = 0;
                for (int j = 1; j < 8 && data[off + j] != ' ' && data[off + j] != 0; j++)
                    num[ni++] = (char)data[off + j];
                num[ni] = '\0';
                uint32_t stroff = (uint32_t)strtoul(num, NULL, 10);
                if (strtab && stroff < strtab_size) {
                    const char *s = (const char *)strtab + stroff;
                    size_t sl = strlen(s);
                    sym->name = malloc(sl + 1);
                    memcpy(sym->name, s, sl + 1);
                } else {
                    sym->name = strdup("?");
                }
            } else {
                char name[9];
                memcpy(name, data + off, 8);
                name[8] = '\0';
                // Trim trailing spaces.
                for (int j = 7; j >= 0 && name[j] == ' '; j--) name[j] = '\0';
                sym->name = strdup(name);
            }
            uint32_t value;
            int16_t sec_num;
            uint16_t stype;
            uint8_t sclass, naux;
            if (read_u32(data, size, off + 8, &value) != 0) { pe_object_free(obj); return 1; }
            // SectionNumber is signed 16-bit.
            if (off + 13 > size) { pe_object_free(obj); return 1; }
            sec_num = (int16_t)((uint16_t)data[off + 12] | ((uint16_t)data[off + 13] << 8));
            if (read_u16(data, size, off + 14, &stype) != 0) { pe_object_free(obj); return 1; }
            sclass = data[off + 16];
            naux = data[off + 17];
            (void)stype;
            sym->value = (int64_t)value;
            sym->section = (sec_num > 0) ? (int)(sec_num - 1) : -1; // convert to 0-based
            sym->global = (sclass == 2); // IMAGE_SYM_CLASS_EXTERNAL
            sym->storage_class = sclass;
            // Skip aux symbols.
            i += naux;
        }
    }

    return 0;
}

void pe_object_free(PeObject *obj) {
    for (size_t i = 0; i < obj->nsection; i++) {
        free(obj->sections[i].data);
        free(obj->sections[i].relocs);
    }
    free(obj->sections);
    for (size_t i = 0; i < obj->nsymbol; i++) {
        free(obj->symbols[i].name);
    }
    free(obj->symbols);
    memset(obj, 0, sizeof(*obj));
}
