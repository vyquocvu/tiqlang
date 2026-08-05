// M17.3.1: minimal Mach-O arm64 relocatable-object writer.
//
// Serializes an AsmUnit (see asm_arm64.c) as an MH_OBJECT file accepted
// by the host linker. Layout is deterministic: header, load commands,
// section contents, relocations, symbol table, string table. No UUIDs,
// timestamps, or environment-dependent bytes are emitted, so repeated
// emission of the same unit is byte-identical.

#include "../include/asm_arm64.h"

#include <stdlib.h>
#include <string.h>

#define MACHO_MAGIC_64 0xFEEDFACFu
#define MACHO_CPU_ARM64 0x0100000Cu
#define MACHO_MH_OBJECT 1u
#define MACHO_SUBSECTIONS_VIA_SYMBOLS 0x2000u
#define MACHO_LC_SEGMENT_64 0x19u
#define MACHO_LC_SYMTAB 0x2u
#define MACHO_N_UNDF 0x00u
#define MACHO_N_SECT 0x0Eu
#define MACHO_N_EXT 0x01u
#define MACHO_S_ATTR_PURE_INSTR 0x80000000u
#define MACHO_S_ATTR_SOME_INSTR 0x00000400u
#define MACHO_S_ZEROFILL 0x1u

typedef struct {
    uint8_t *bytes;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < b->len + n) cap *= 2;
        b->bytes = realloc(b->bytes, cap);
        b->cap = cap;
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void buf_u32(Buf *b, uint32_t v) { buf_put(b, &v, 4); }
static void buf_u64(Buf *b, uint64_t v) { buf_put(b, &v, 8); }

static void buf_pad(Buf *b, size_t align) {
    while (b->len % align) {
        uint8_t z = 0;
        buf_put(b, &z, 1);
    }
}

// Fixed-width name field (16 bytes, zero-padded).
static void buf_name(Buf *b, const char *s) {
    char field[16];
    memset(field, 0, sizeof(field));
    strncpy(field, s, 15);
    buf_put(b, field, 16);
}

// section_64 (80 bytes).
static void emit_section(Buf *b, const char *sect, const char *seg, uint64_t addr,
                         uint64_t size, uint32_t offset, uint32_t align,
                         uint32_t reloff, uint32_t nreloc, uint32_t flags) {
    buf_name(b, sect);
    buf_name(b, seg);
    buf_u64(b, addr);        // section address relative to segment vmaddr
    buf_u64(b, size);
    buf_u32(b, offset);
    buf_u32(b, align);
    buf_u32(b, reloff);
    buf_u32(b, nreloc);
    buf_u32(b, flags);
    buf_u32(b, 0);           // reserved1
    buf_u32(b, 0);           // reserved2
    buf_u32(b, 0);           // reserved3
}

// segment_command_64 header (72 bytes, before its sections).
static void emit_segment(Buf *b, const char *seg, uint64_t vmsize, uint64_t fileoff,
                         uint64_t filesize, uint32_t nsects, int32_t initprot) {
    buf_u32(b, MACHO_LC_SEGMENT_64);
    buf_u32(b, 72 + 80 * nsects);
    buf_name(b, seg);
    buf_u64(b, 0);           // vmaddr: assigned by the linker
    buf_u64(b, vmsize);
    buf_u64(b, fileoff);
    buf_u64(b, filesize);
    buf_u32(b, 7);           // maxprot: rwx
    buf_u32(b, (uint32_t)initprot);
    buf_u32(b, nsects);
    buf_u32(b, 0);           // flags
}

static void emit_relocs(Buf *b, const AsmSectionOut *s) {
    for (size_t i = 0; i < s->nreloc; i++) {
        const AsmReloc *r = &s->relocs[i];
        buf_u32(b, (uint32_t)r->address);
        uint32_t packed = (uint32_t)r->symbol |
                          (r->pcrel ? 1u << 24 : 0) |
                          ((r->length & 3u) << 25) |
                          (1u << 27) |                 // r_extern: always
                          ((r->type & 0xFu) << 28);
        buf_u32(b, packed);
    }
}

int macho_obj_write(const AsmUnit *u, FILE *out) {
    int has_data = u->sec[ASM_SEC_DATA].used || u->sec[ASM_SEC_DATA].len > 0;
    int has_bss = u->sec[ASM_SEC_BSS].used && u->sec[ASM_SEC_BSS].len > 0;

    const AsmSectionOut *text = &u->sec[ASM_SEC_TEXT];
    const AsmSectionOut *data = &u->sec[ASM_SEC_DATA];
    const AsmSectionOut *bss = &u->sec[ASM_SEC_BSS];

    // 1-based Mach-O section ordinals in load-command order.
    int sect_ord[ASM_SEC_COUNT] = { 0, 0, 0, 0 };
    int ord = 1;
    sect_ord[ASM_SEC_TEXT] = ord++;
    if (has_data) sect_ord[ASM_SEC_DATA] = ord++;
    if (has_bss) sect_ord[ASM_SEC_BSS] = ord++;

    // String table: leading NUL, then every symbol name NUL-terminated.
    size_t strsize = 1;
    size_t *stroff = malloc(u->nsym * sizeof(size_t));
    for (size_t i = 0; i < u->nsym; i++) {
        stroff[i] = strsize;
        strsize += strlen(u->syms[i].name) + 1;
    }

    // Layout: header + load commands, then contents.
    // clang packs __text and __data into a single anonymous segment so the
    // vm ranges never overlap; data content starts right after text at an
    // 8-byte aligned offset, and the __data section address follows suit.
    uint32_t nsects = 1 + (has_data ? 1u : 0) + (has_bss ? 1u : 0);
    size_t sizeofcmds = 72 + 80 * (size_t)nsects + 24; // segment + LC_SYMTAB
    size_t header_size = 32 + sizeofcmds;

    size_t text_off = (header_size + 7) & ~(size_t)7;
    size_t data_off = (text_off + text->len + 7) & ~(size_t)7;
    uint64_t data_addr = (text->len + 7) & ~(uint64_t)7;
    size_t reloc_text_off = (data_off + (has_data ? data->len : 0) + 7) & ~(size_t)7;
    size_t reloc_data_off = reloc_text_off + 8 * text->nreloc;
    size_t symoff = reloc_data_off + 8 * (has_data ? data->nreloc : 0);
    size_t stroff_file = symoff + 16 * u->nsym;

    uint64_t vmsize = (text->len + 7) & ~(uint64_t)7;
    if (has_data) vmsize = (data_addr + data->len + 7) & ~(uint64_t)7;
    uint64_t bss_addr = vmsize;
    if (has_bss) vmsize += bss->len;
    uint64_t filesize = (has_data ? data_off + data->len : text_off + text->len) - text_off;

    Buf b = { 0 };

    // --- mach_header_64 ---
    buf_u32(&b, MACHO_MAGIC_64);
    buf_u32(&b, MACHO_CPU_ARM64);
    buf_u32(&b, 0); // cpusubtype: CPU_SUBTYPE_ARM64_ALL
    buf_u32(&b, MACHO_MH_OBJECT);
    buf_u32(&b, 2); // ncmds: one segment + LC_SYMTAB
    buf_u32(&b, (uint32_t)sizeofcmds);
    buf_u32(&b, MACHO_SUBSECTIONS_VIA_SYMBOLS);
    buf_u32(&b, 0); // reserved

    // --- single anonymous segment holding all sections (clang layout) ---
    emit_segment(&b, "", vmsize, text_off, filesize, nsects, 7);
    emit_section(&b, "__text", "__TEXT", 0, text->len, (uint32_t)text_off, 2,
                 (uint32_t)reloc_text_off, (uint32_t)text->nreloc,
                 MACHO_S_ATTR_PURE_INSTR | MACHO_S_ATTR_SOME_INSTR);
    if (has_data)
        emit_section(&b, "__data", "__DATA", data_addr, data->len,
                     (uint32_t)data_off, 3,
                     (uint32_t)reloc_data_off, (uint32_t)data->nreloc, 0);
    if (has_bss)
        emit_section(&b, "__bss", "__DATA", bss_addr, bss->len,
                     0, 3, 0, 0, MACHO_S_ZEROFILL);

    // --- LC_SYMTAB ---
    buf_u32(&b, MACHO_LC_SYMTAB);
    buf_u32(&b, 24);
    buf_u32(&b, (uint32_t)symoff);
    buf_u32(&b, (uint32_t)u->nsym);
    buf_u32(&b, (uint32_t)stroff_file);
    buf_u32(&b, (uint32_t)strsize);

    // --- section contents ---
    buf_pad(&b, 8);
    buf_put(&b, text->bytes, text->len);
    if (has_data) {
        buf_pad(&b, 8);
        buf_put(&b, data->bytes, data->len);
    }

    // --- relocations ---
    buf_pad(&b, 8);
    emit_relocs(&b, text);
    if (has_data) emit_relocs(&b, data);

    // --- symbol table + string table ---
    // n_value is segment-relative, so section-local offsets must be shifted
    // by the section's address within the segment.
    uint64_t sect_addr[ASM_SEC_COUNT] = { 0, data_addr, bss_addr, 0 };
    buf_pad(&b, 8);
    for (size_t i = 0; i < u->nsym; i++) {
        const AsmSymbol *s = &u->syms[i];
        buf_u32(&b, (uint32_t)stroff[i]);
        uint8_t ntype = s->section >= 0 ? (MACHO_N_SECT | MACHO_N_EXT)
                                        : (MACHO_N_UNDF | MACHO_N_EXT);
        buf_put(&b, &ntype, 1);
        uint8_t nsect = s->section >= 0 ? (uint8_t)sect_ord[s->section] : 0;
        buf_put(&b, &nsect, 1);
        uint16_t ndesc = 0;
        buf_put(&b, &ndesc, 2);
        buf_u64(&b, s->section >= 0 ? sect_addr[s->section] + (uint64_t)s->value : 0);
    }
    {
        uint8_t z = 0;
        buf_put(&b, &z, 1);
        for (size_t i = 0; i < u->nsym; i++)
            buf_put(&b, u->syms[i].name, strlen(u->syms[i].name) + 1);
    }

    free(stroff);
    size_t written = fwrite(b.bytes, 1, b.len, out);
    free(b.bytes);
    return written == b.len ? 0 : 1;
}
