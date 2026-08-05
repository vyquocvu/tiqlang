// M17.3.3: minimal ELF64 aarch64 relocatable-object writer.
//
// Serializes an AsmUnit (see asm_arm64.c, fmt == ASM_FMT_ELF) as an
// ET_REL ELF64 file accepted by the host linker. Layout is
// deterministic: header, section contents, relocations, symbol table,
// string tables, section headers. No timestamps or environment-
// dependent bytes are emitted, so repeated emission of the same unit
// is byte-identical.

#include "../include/asm_arm64.h"
#include "../include/elf_link.h"

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
        b->bytes = realloc(b->bytes, cap);
        b->cap = cap;
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void buf_le16(Buf *b, uint16_t v) { uint8_t d[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; buf_put(b, d, 2); }
static void buf_le32(Buf *b, uint32_t v) { uint8_t d[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) }; buf_put(b, d, 4); }
static void buf_le64(Buf *b, uint64_t v) { uint8_t d[8]; for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (8*i)); buf_put(b, d, 8); }

static void buf_pad(Buf *b, size_t align) {
    while (b->len % align) { uint8_t z = 0; buf_put(b, &z, 1); }
}

// Translate internal relocation type to ELF aarch64 type.
static uint32_t elf_reloc_type(int internal_type) {
    switch (internal_type) {
        case ASM_RELOC_UNSIGNED:   return R_AARCH64_ABS64;
        case ASM_RELOC_BRANCH26:   return R_AARCH64_CALL26;
        case ASM_RELOC_PAGE21:     return R_AARCH64_ADR_PREL_PG_HI21;
        case ASM_RELOC_PAGEOFF12:  return R_AARCH64_ADD_ABS_LO12_NC;
        default:                   return R_AARCH64_NONE;
    }
}

// Emit an ELF64 section header (64 bytes).
static void emit_shdr(Buf *b, uint32_t name_off, uint32_t type, uint64_t flags,
                       uint64_t addr, uint64_t offset, uint64_t size,
                       uint32_t link, uint32_t info, uint64_t align,
                       uint64_t entsize) {
    buf_le32(b, name_off);
    buf_le32(b, type);
    buf_le64(b, flags);
    buf_le64(b, addr);
    buf_le64(b, offset);
    buf_le64(b, size);
    buf_le32(b, link);
    buf_le32(b, info);
    buf_le64(b, align);
    buf_le64(b, entsize);
}

int elf_obj_write(const AsmUnit *u, FILE *out) {
    int has_data = u->sec[ASM_SEC_DATA].used || u->sec[ASM_SEC_DATA].len > 0;
    int has_bss = u->sec[ASM_SEC_BSS].used && u->sec[ASM_SEC_BSS].len > 0;
    int has_rodata = u->sec[ASM_SEC_RODATA].used || u->sec[ASM_SEC_RODATA].len > 0;
    int has_text_relocs = u->sec[ASM_SEC_TEXT].nreloc > 0;
    int has_data_relocs = has_data && u->sec[ASM_SEC_DATA].nreloc > 0;

    // Build section name string table (.shstrtab).
    // Layout: \0 .text \0 .data \0 .bss \0 .rodata \0 .rela.text \0 .rela.data \0
    //         .symtab \0 .strtab \0 .shstrtab \0 .note.GNU-stack \0
    Buf shstrtab = { 0 };
    { uint8_t z = 0; buf_put(&shstrtab, &z, 1); }

    // Record name offsets for each section we emit.
    uint32_t name_text = (uint32_t)shstrtab.len;
    buf_put(&shstrtab, ".text", sizeof(".text"));
    uint32_t name_data = 0;
    if (has_data) { name_data = (uint32_t)shstrtab.len; buf_put(&shstrtab, ".data", sizeof(".data")); }
    uint32_t name_bss = 0;
    if (has_bss) { name_bss = (uint32_t)shstrtab.len; buf_put(&shstrtab, ".bss", sizeof(".bss")); }
    uint32_t name_rodata = 0;
    if (has_rodata) { name_rodata = (uint32_t)shstrtab.len; buf_put(&shstrtab, ".rodata", sizeof(".rodata")); }
    uint32_t name_rela_text = 0;
    if (has_text_relocs) { name_rela_text = (uint32_t)shstrtab.len; buf_put(&shstrtab, ".rela.text", sizeof(".rela.text")); }
    uint32_t name_rela_data = 0;
    if (has_data_relocs) { name_rela_data = (uint32_t)shstrtab.len; buf_put(&shstrtab, ".rela.data", sizeof(".rela.data")); }
    uint32_t name_symtab = (uint32_t)shstrtab.len;
    buf_put(&shstrtab, ".symtab", sizeof(".symtab"));
    uint32_t name_strtab = (uint32_t)shstrtab.len;
    buf_put(&shstrtab, ".strtab", sizeof(".strtab"));
    uint32_t name_shstrtab = (uint32_t)shstrtab.len;
    buf_put(&shstrtab, ".shstrtab", sizeof(".shstrtab"));
    uint32_t name_gnu_stack = 0;
    if (u->has_gnu_stack) { name_gnu_stack = (uint32_t)shstrtab.len; buf_put(&shstrtab, ".note.GNU-stack", sizeof(".note.GNU-stack")); }

    // Build symbol string table (.strtab).
    Buf strtab = { 0 };
    { uint8_t z = 0; buf_put(&strtab, &z, 1); }
    size_t *sym_strtab_off = malloc(u->nsym * sizeof(size_t));
    for (size_t i = 0; i < u->nsym; i++) {
        sym_strtab_off[i] = strtab.len;
        buf_put(&strtab, u->syms[i].name, strlen(u->syms[i].name) + 1);
    }

    // Count sections: NULL + text + data? + bss? + rodata? + rela.text? +
    // rela.data? + symtab + strtab + shstrtab + gnu-stack?
    uint32_t shnum = 1; // SHT_NULL
    uint32_t sh_text = shnum++;
    uint32_t sh_data = has_data ? shnum++ : 0;
    uint32_t sh_bss = has_bss ? shnum++ : 0;
    uint32_t sh_rodata = has_rodata ? shnum++ : 0;
    uint32_t sh_rela_text = has_text_relocs ? shnum++ : 0;
    uint32_t sh_rela_data = has_data_relocs ? shnum++ : 0;
    uint32_t sh_symtab = shnum++;
    uint32_t sh_strtab = shnum++;
    uint32_t sh_shstrtab = shnum++;
    uint32_t sh_gnu_stack = u->has_gnu_stack ? shnum++ : 0;
    (void)sh_rela_text; (void)sh_rela_data; (void)sh_gnu_stack;

    // Section ordinals for symbol table (1-based, matching section header order).
    uint32_t sect_ord[ASM_SEC_COUNT] = { 0, 0, 0, 0 };
    sect_ord[ASM_SEC_TEXT] = sh_text;
    if (has_data) sect_ord[ASM_SEC_DATA] = sh_data;
    if (has_bss) sect_ord[ASM_SEC_BSS] = sh_bss;
    if (has_rodata) sect_ord[ASM_SEC_RODATA] = sh_rodata;

    // Layout: ELF header (64 bytes), then section contents, then symtab/strtab,
    // then section headers at end.
    size_t header_size = 64;

    // Section content offsets (aligned).
    size_t text_off = header_size;
    const AsmSectionOut *text = &u->sec[ASM_SEC_TEXT];
    size_t cur_off = text_off + text->len;

    size_t rodata_off = 0;
    const AsmSectionOut *rodata = &u->sec[ASM_SEC_RODATA];
    if (has_rodata) {
        cur_off = (cur_off + 7) & ~(size_t)7;
        rodata_off = cur_off;
        cur_off += rodata->len;
    }

    size_t data_off = 0;
    const AsmSectionOut *data = &u->sec[ASM_SEC_DATA];
    if (has_data) {
        cur_off = (cur_off + 7) & ~(size_t)7;
        data_off = cur_off;
        cur_off += data->len;
    }

    // Relocation sections.
    size_t rela_text_off = (cur_off + 7) & ~(size_t)7;
    cur_off = rela_text_off + 24 * text->nreloc; // Elf64_Rela = 24 bytes

    size_t rela_data_off = 0;
    if (has_data_relocs) {
        cur_off = (cur_off + 7) & ~(size_t)7;
        rela_data_off = cur_off;
        cur_off += 24 * u->sec[ASM_SEC_DATA].nreloc;
    }

    // Symbol table: symtab has a leading STT_NOTYPE local symbol.
    uint32_t nsym_elf = (uint32_t)u->nsym + 1; // +1 for the null symbol
    // Count locals (non-global) for sh_info.
    uint32_t first_global = 1; // after the null symbol
    for (size_t i = 0; i < u->nsym; i++) {
        if (!u->syms[i].global) first_global++;
    }

    size_t symtab_off = (cur_off + 7) & ~(size_t)7;
    cur_off = symtab_off + 24 * nsym_elf; // Elf64_Sym = 24 bytes

    size_t strtab_off = (cur_off + 7) & ~(size_t)7;
    cur_off = strtab_off + strtab.len;

    size_t shstrtab_off = (cur_off + 7) & ~(size_t)7;
    cur_off = shstrtab_off + shstrtab.len;

    // GNU-stack note section (4 bytes of zero data, aligned).
    size_t gnu_stack_off = 0;
    if (u->has_gnu_stack) {
        cur_off = (cur_off + 7) & ~(size_t)7;
        gnu_stack_off = cur_off;
        cur_off += 4; // 4 bytes of zeros
    }

    // Section headers at end.
    size_t shdr_off = (cur_off + 7) & ~(size_t)7;

    // Build the output.
    Buf b = { 0 };

    // --- ELF header (64 bytes) ---
    buf_put(&b, "\x7f" "ELF", 4);  // e_ident[EI_MAG]
    buf_put(&b, "\x02\x01\x01\x00", 4); // ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE
    buf_put(&b, "\x00\x00\x00\x00\x00\x00\x00\x00", 8); // padding
    buf_le16(&b, ET_REL);           // e_type
    buf_le16(&b, EM_AARCH64);       // e_machine
    buf_le32(&b, EV_CURRENT);       // e_version
    buf_le64(&b, 0);                // e_entry
    buf_le64(&b, 0);                // e_phoff (no program headers for ET_REL)
    buf_le64(&b, (uint64_t)shdr_off); // e_shoff
    buf_le32(&b, 0);                // e_flags
    buf_le16(&b, 64);               // e_ehsize
    buf_le16(&b, 0);                // e_phentsize
    buf_le16(&b, 0);                // e_phnum
    buf_le16(&b, 64);               // e_shentsize
    buf_le16(&b, (uint16_t)shnum);  // e_shnum
    buf_le16(&b, (uint16_t)sh_shstrtab); // e_shstrndx

    // --- Section contents ---
    // .text
    buf_pad(&b, 8);
    if (text->len > 0) buf_put(&b, text->bytes, text->len);

    // .rodata
    if (has_rodata) {
        buf_pad(&b, 8);
        buf_put(&b, rodata->bytes, rodata->len);
    }

    // .data
    if (has_data) {
        buf_pad(&b, 8);
        buf_put(&b, data->bytes, data->len);
    }

    // .rela.text
    if (has_text_relocs) {
        buf_pad(&b, 8);
        for (size_t i = 0; i < text->nreloc; i++) {
            const AsmReloc *r = &text->relocs[i];
            buf_le64(&b, (uint64_t)r->address);
            uint64_t info = ((uint64_t)(r->symbol + 1) << 32) |
                            (uint64_t)elf_reloc_type(r->type);
            buf_le64(&b, info);
            buf_le64(&b, (uint64_t)0); // addend: encoded in instruction for PC-rel
        }
    }

    // .rela.data
    if (has_data_relocs) {
        buf_pad(&b, 8);
        const AsmSectionOut *ds = &u->sec[ASM_SEC_DATA];
        for (size_t i = 0; i < ds->nreloc; i++) {
            const AsmReloc *r = &ds->relocs[i];
            buf_le64(&b, (uint64_t)r->address);
            uint64_t info = ((uint64_t)(r->symbol + 1) << 32) |
                            (uint64_t)elf_reloc_type(r->type);
            buf_le64(&b, info);
            buf_le64(&b, (uint64_t)0);
        }
    }

    // .symtab
    buf_pad(&b, 8);
    // Null symbol (index 0).
    for (int i = 0; i < 24; i++) buf_put(&b, "\x00", 1);

    // Emit locals first, then globals (ELF requires this ordering).
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < u->nsym; i++) {
            const AsmSymbol *s = &u->syms[i];
            int is_global = s->global != 0;
            if (pass == 0 && is_global) continue;
            if (pass == 1 && !is_global) continue;

            buf_le32(&b, (uint32_t)sym_strtab_off[i]); // st_name
            uint8_t st_info = (uint8_t)((is_global ? STB_GLOBAL : STB_LOCAL) << 4 | STT_NOTYPE);
            buf_put(&b, &st_info, 1);                   // st_info
            uint8_t st_other = 0;
            buf_put(&b, &st_other, 1);                   // st_other
            uint16_t st_shndx = s->section >= 0 ? (uint16_t)sect_ord[s->section] : 0;
            if (s->section < 0) st_shndx = 0; // SHN_UNDEF
            buf_le16(&b, st_shndx);                      // st_shndx
            buf_le64(&b, (uint64_t)s->value);            // st_value
            buf_le64(&b, 0);                             // st_size
        }
    }

    // .strtab
    buf_pad(&b, 8);
    buf_put(&b, strtab.bytes, strtab.len);

    // .shstrtab
    buf_pad(&b, 8);
    buf_put(&b, shstrtab.bytes, shstrtab.len);

    // .note.GNU-stack
    if (u->has_gnu_stack) {
        buf_pad(&b, 8);
        uint8_t zeros[4] = { 0, 0, 0, 0 };
        buf_put(&b, zeros, 4);
    }

    // --- Section headers ---
    buf_pad(&b, 8);

    // [0] SHT_NULL
    emit_shdr(&b, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);

    // .text
    emit_shdr(&b, name_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
              0, text_off, text->len, 0, 0, 4, 0);

    // .data
    if (has_data)
        emit_shdr(&b, name_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
                  0, data_off, data->len, 0, 0, 8, 0);

    // .bss
    if (has_bss)
        emit_shdr(&b, name_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
                  0, 0, u->sec[ASM_SEC_BSS].len, 0, 0, 8, 0);

    // .rodata
    if (has_rodata)
        emit_shdr(&b, name_rodata, SHT_PROGBITS, SHF_ALLOC,
                  0, rodata_off, rodata->len, 0, 0, 8, 0);

    // .rela.text
    if (has_text_relocs)
        emit_shdr(&b, name_rela_text, SHT_RELA, 0,
                  0, rela_text_off, 24 * text->nreloc,
                  sh_symtab, sh_text, 8, 24);

    // .rela.data
    if (has_data_relocs)
        emit_shdr(&b, name_rela_data, SHT_RELA, 0,
                  0, rela_data_off, 24 * u->sec[ASM_SEC_DATA].nreloc,
                  sh_symtab, sh_data, 8, 24);

    // .symtab
    emit_shdr(&b, name_symtab, SHT_SYMTAB, 0,
              0, symtab_off, 24 * nsym_elf,
              sh_strtab, first_global, 8, 24);

    // .strtab
    emit_shdr(&b, name_strtab, SHT_STRTAB, 0,
              0, strtab_off, strtab.len, 0, 0, 1, 0);

    // .shstrtab
    emit_shdr(&b, name_shstrtab, SHT_STRTAB, 0,
              0, shstrtab_off, shstrtab.len, 0, 0, 1, 0);

    // .note.GNU-stack
    if (u->has_gnu_stack)
        emit_shdr(&b, name_gnu_stack, SHT_PROGBITS, 0,
                  0, gnu_stack_off, 4, 0, 0, 1, 0);

    free(sym_strtab_off);
    free(strtab.bytes);
    free(shstrtab.bytes);

    size_t written = fwrite(b.bytes, 1, b.len, out);
    free(b.bytes);
    return written == b.len ? 0 : 1;
}
