// M17.3.4: minimal PE/COFF relocatable-object writer for x86_64.
//
// Serializes an AsmUnit (see asm_amd64.c, fmt == ASM_FMT_PE) as a
// PE/COFF ET_REL file. Layout is deterministic: COFF header, section
// headers, section contents, relocations, symbol table, string table.
// No timestamps or environment-dependent bytes are emitted.

#include "../include/asm_amd64.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *bytes;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < b->len + n) cap *= 2;
        void *tmp = realloc(b->bytes, cap);
        if (!tmp) { fprintf(stderr, "pe_obj: out of memory\n"); exit(1); }
        b->bytes = tmp;
        b->cap = cap;
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void buf_le16(Buf *b, uint16_t v) { uint8_t d[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; buf_put(b, d, 2); }
static void buf_le32(Buf *b, uint32_t v) { uint8_t d[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) }; buf_put(b, d, 4); }

static void buf_pad(Buf *b, size_t align) {
    while (b->len % align) { uint8_t z = 0; buf_put(b, &z, 1); }
}

// Section characteristics.
#define IMAGE_SCN_CNT_CODE               0x00000020u
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040u
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#define IMAGE_SCN_MEM_EXECUTE            0x20000000u
#define IMAGE_SCN_MEM_READ               0x40000000u
#define IMAGE_SCN_MEM_WRITE              0x80000000u
#define IMAGE_SCN_ALIGN_16BYTES          0x00500000u
#define IMAGE_SCN_ALIGN_4BYTES           0x00300000u

// COFF storage classes.
#define IMAGE_SYM_CLASS_EXTERNAL 2
#define IMAGE_SYM_CLASS_STATIC   3

// Translate internal relocation type to PE/COFF AMD64 type.
static uint16_t pe_reloc_type(int internal_type) {
    switch (internal_type) {
        case ASM_RELOC_X86_64_64:          return 0x0001; // IMAGE_REL_AMD64_ADDR64
        case ASM_RELOC_X86_64_PC32:        return 0x0004; // IMAGE_REL_AMD64_REL32
        case ASM_RELOC_X86_64_GOTPCRELX:   return 0x0005; // IMAGE_REL_AMD64_REL32_1
        default:                            return 0x0004; // default to REL32
    }
}

// Map ASM_SEC_* to PE section name and characteristics.
static const char *pe_section_name(int idx) {
    switch (idx) {
        case ASM_SEC_TEXT:   return ".text";
        case ASM_SEC_DATA:   return ".data";
        case ASM_SEC_BSS:    return ".bss";
        case ASM_SEC_RODATA: return ".rdata";
        default:             return "";
    }
}

static uint32_t pe_section_chars(int idx) {
    switch (idx) {
        case ASM_SEC_TEXT:
            return IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES;
        case ASM_SEC_DATA:
            return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_ALIGN_4BYTES;
        case ASM_SEC_BSS:
            return IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_ALIGN_4BYTES;
        case ASM_SEC_RODATA:
            return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_4BYTES;
        default:
            return 0;
    }
}

// Write a section name into the 8-byte COFF section header name field.
// Short names (≤8 chars including NUL) go inline; longer names use /N
// string table references (not needed for our standard section names).
static void write_section_name(Buf *b, const char *name) {
    uint8_t field[8];
    memset(field, 0, 8);
    size_t l = strlen(name);
    if (l <= 8) memcpy(field, name, l);
    else {
        // /N format — should not happen for our standard names.
        char num[32];
        snprintf(num, sizeof(num), "/%zu", l); // approximate
        memcpy(field, num, 8);
    }
    buf_put(b, field, 8);
}

// Write a symbol name into the 8-byte COFF symbol table name field.
// Short names (≤8 chars) go inline; longer names use /N offset into
// the string table. Returns the string table offset if a long name was
// written (caller appends to strtab); 0 for inline names.
static size_t write_symbol_name(Buf *b, const char *name, size_t name_len, Buf *strtab) {
    uint8_t field[8];
    memset(field, 0, 8);
    if (name_len <= 8) {
        memcpy(field, name, name_len);
        buf_put(b, field, 8);
        return 0;
    }
    // Long name: write /offset.
    uint32_t off = (uint32_t)strtab->len;
    buf_put(b, "/", 1);
    char num[8];
    int nl = snprintf(num, sizeof(num), "%u", off);
    buf_put(b, num, (size_t)nl);
    // Pad to 8 bytes.
    for (int i = nl + 1; i < 8; i++) buf_put(b, " ", 1);
    // Append actual name to string table.
    buf_put(strtab, name, name_len);
    buf_put(strtab, "\0", 1);
    return off;
}

int pe_obj_write(const AsmUnit *u, FILE *out) {
    // Determine which sections are used.
    int sec_order[ASM_SEC_COUNT];
    int nsec = 0;
    for (int i = 0; i < ASM_SEC_COUNT; i++) {
        if (u->sec[i].used || (i != ASM_SEC_BSS && u->sec[i].len > 0) ||
            (i == ASM_SEC_BSS && u->sec[i].len > 0)) {
            sec_order[nsec++] = i;
        }
    }
    if (nsec == 0) return 1;

    // Build string table for long symbol names.
    Buf strtab;
    memset(&strtab, 0, sizeof(strtab));
    uint8_t strtab_size[4] = {4, 0, 0, 0}; // minimum size = 4
    buf_put(&strtab, strtab_size, 4);

    // Count symbols: section symbols + user symbols.
    int nsym = nsec; // section symbols
    for (size_t i = 0; i < u->nsym; i++) {
        if (u->syms[i].name[0] != '\0') nsym++;
    }

    // Compute layout.
    // COFF header: 20 bytes.
    // Section headers: nsec * 40 bytes.
    // Section data: aligned.
    // Relocations: per section.
    // Symbol table: nsym * 18 bytes.
    // String table: strtab.len bytes.

    Buf out_buf;
    memset(&out_buf, 0, sizeof(out_buf));

    // COFF header (20 bytes).
    uint32_t header_size = 20;
    uint32_t sec_hdrs_size = (uint32_t)(nsec * 40);
    uint32_t data_start = header_size + sec_hdrs_size;

    // We'll fill in PointerToSymbolTable after we know the layout.
    buf_le16(&out_buf, 0x8664);          // Machine: AMD64
    buf_le16(&out_buf, (uint16_t)nsec);   // NumberOfSections
    buf_le32(&out_buf, 0);               // TimeDateStamp: 0 (deterministic)
    uint32_t symtab_off_placeholder = (uint32_t)out_buf.len;
    buf_le32(&out_buf, 0);               // PointerToSymbolTable (placeholder)
    buf_le32(&out_buf, (uint32_t)nsym);   // NumberOfSymbols
    buf_le16(&out_buf, 0);               // SizeOfOptionalHeader: 0
    buf_le16(&out_buf, 0);               // Characteristics

    // Write section headers (40 bytes each).
    // We need to track where each section's data and relocations go.
    uint32_t sec_data_off[ASM_SEC_COUNT];
    uint32_t sec_reloc_off[ASM_SEC_COUNT];
    uint32_t cur_off = data_start;

    for (int i = 0; i < nsec; i++) {
        int si = sec_order[i];
        const AsmSectionOut *sec = &u->sec[si];
        sec_data_off[si] = cur_off;
        cur_off += (uint32_t)sec->len;
        // Align to 4 bytes.
        while (cur_off % 4) cur_off++;
        sec_reloc_off[si] = cur_off;
        cur_off += (uint32_t)(sec->nreloc * 10); // 10 bytes per relocation
    }

    // Section headers.
    for (int i = 0; i < nsec; i++) {
        int si = sec_order[i];
        const AsmSectionOut *sec = &u->sec[si];
        write_section_name(&out_buf, pe_section_name(si));
        buf_le32(&out_buf, (uint32_t)sec->len);  // VirtualSize
        buf_le32(&out_buf, 0);                     // VirtualAddress
        buf_le32(&out_buf, (uint32_t)sec->len);   // SizeOfRawData
        buf_le32(&out_buf, sec_data_off[si]);      // PointerToRawData
        buf_le32(&out_buf, sec->nreloc > 0 ? sec_reloc_off[si] : 0); // PointerToRelocations
        buf_le32(&out_buf, 0);                     // PointerToLinenumbers
        buf_le16(&out_buf, (uint16_t)sec->nreloc); // NumberOfRelocations
        buf_le16(&out_buf, 0);                     // NumberOfLinenumbers
        buf_le32(&out_buf, pe_section_chars(si));   // Characteristics
    }

    // Write section data and relocations.
    for (int i = 0; i < nsec; i++) {
        int si = sec_order[i];
        const AsmSectionOut *sec = &u->sec[si];
        // Section data.
        if (sec->len > 0) buf_put(&out_buf, sec->bytes, sec->len);
        buf_pad(&out_buf, 4);
        // Relocations (10 bytes each).
        for (size_t r = 0; r < sec->nreloc; r++) {
            const AsmReloc *rel = &sec->relocs[r];
            // Map internal symbol index to COFF symbol table index.
            // Section symbols come first (one per section), then user symbols.
            int coff_sym;
            if (rel->symbol >= 0 && rel->symbol < (int)u->nsym) {
                // Find the user symbol's position in the COFF symbol table.
                coff_sym = nsec; // start of user symbols
                for (int j = 0; j < (int)u->nsym; j++) {
                    if (j == rel->symbol) break;
                    if (u->syms[j].name[0] != '\0') coff_sym++;
                }
            } else {
                coff_sym = 0;
            }
            buf_le32(&out_buf, (uint32_t)rel->address);  // VirtualAddress
            buf_le32(&out_buf, (uint32_t)coff_sym);       // SymbolTableIndex
            buf_le16(&out_buf, pe_reloc_type(rel->type)); // Type
        }
    }

    // Symbol table.
    uint32_t symtab_off = (uint32_t)out_buf.len;
    // Section symbols first.
    for (int i = 0; i < nsec; i++) {
        int si = sec_order[i];
        write_section_name(&out_buf, pe_section_name(si));
        buf_le32(&out_buf, 0);                               // Value
        buf_le16(&out_buf, (uint16_t)(i + 1));               // SectionNumber (1-based)
        buf_le16(&out_buf, 0);                               // Type
        buf_put(&out_buf, &(uint8_t){IMAGE_SYM_CLASS_STATIC}, 1); // StorageClass
        buf_put(&out_buf, &(uint8_t){0}, 1);                 // NumberOfAuxSymbols
    }
    // User symbols.
    for (size_t i = 0; i < u->nsym; i++) {
        const AsmSymbol *sym = &u->syms[i];
        if (sym->name[0] == '\0') continue;
        size_t nlen = strlen(sym->name);
        write_symbol_name(&out_buf, sym->name, nlen, &strtab);
        buf_le32(&out_buf, (uint32_t)sym->value);            // Value
        if (sym->section >= 0 && sym->section < ASM_SEC_COUNT) {
            // Find 1-based section number.
            int sec_num = 0;
            for (int j = 0; j < nsec; j++) {
                if (sec_order[j] == sym->section) { sec_num = j + 1; break; }
            }
            buf_le16(&out_buf, (uint16_t)sec_num);
        } else {
            buf_le16(&out_buf, 0); // undefined
        }
        buf_le16(&out_buf, 0);  // Type
        uint8_t sc = sym->global ? IMAGE_SYM_CLASS_EXTERNAL : IMAGE_SYM_CLASS_STATIC;
        buf_put(&out_buf, &sc, 1); // StorageClass
        buf_put(&out_buf, &(uint8_t){0}, 1); // NumberOfAuxSymbols
    }

    // Patch PointerToSymbolTable in the COFF header.
    uint8_t *p = out_buf.bytes + symtab_off_placeholder;
    p[0] = (uint8_t)symtab_off;
    p[1] = (uint8_t)(symtab_off >> 8);
    p[2] = (uint8_t)(symtab_off >> 16);
    p[3] = (uint8_t)(symtab_off >> 24);

    // String table: patch size.
    uint32_t strtab_total = (uint32_t)strtab.len;
    strtab.bytes[0] = (uint8_t)strtab_total;
    strtab.bytes[1] = (uint8_t)(strtab_total >> 8);
    strtab.bytes[2] = (uint8_t)(strtab_total >> 16);
    strtab.bytes[3] = (uint8_t)(strtab_total >> 24);

    // Write everything.
    int ok = 1;
    if (fwrite(out_buf.bytes, 1, out_buf.len, out) != out_buf.len) ok = 0;
    if (ok && strtab.len > 4) {
        if (fwrite(strtab.bytes, 1, strtab.len, out) != strtab.len) ok = 0;
    }

    free(out_buf.bytes);
    free(strtab.bytes);
    return ok ? 0 : 1;
}
