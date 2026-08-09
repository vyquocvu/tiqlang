// M17.3.4: ELF64 relocatable-object reader (aarch64 + x86_64).
//
// Parses the subset of ELF64 relocatable objects produced by the
// integrated writer (elf_obj.c) and by cc for the QBE runtime
// (build/runtime_qbe.o). Anything unsupported fails closed with a
// diagnostic.

#include "../include/elf_link.h"
#include "../include/asm_arm64.h"

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

static int read_u64(const uint8_t *p, size_t end, size_t off, uint64_t *out) {
    if (off + 8 > end) return -1;
    *out = 0;
    for (int i = 0; i < 8; i++) *out |= (uint64_t)p[off + i] << (8 * i);
    return 0;
}

static int read_i64(const uint8_t *p, size_t end, size_t off, int64_t *out) {
    uint64_t v;
    if (read_u64(p, end, off, &v) != 0) return -1;
    *out = (int64_t)v;
    return 0;
}

// Translate ELF relocation type to internal representation based on machine.
static int internal_reloc_type(uint16_t machine, uint32_t elf_type) {
    if (machine == EM_RISCV) {
        switch (elf_type) {
            case R_RISCV_32:           return ASM_RELOC_RISCV_32;
            case R_RISCV_64:           return ASM_RELOC_RISCV_64;
            case R_RISCV_PCREL_HI20:   return ASM_RELOC_RISCV_HI20;
            case R_RISCV_PCREL_LO12_I: return ASM_RELOC_RISCV_LO12_I;
            case R_RISCV_PCREL_LO12_S: return ASM_RELOC_RISCV_LO12_S;
            default:                    return -1;
        }
    }
    if (machine == EM_X86_64) {
        switch (elf_type) {
            case R_X86_64_64:          return ASM_RELOC_X86_64_64;
            case R_X86_64_PC32:        return ASM_RELOC_X86_64_PC32;
            case R_X86_64_PLT32:       return ASM_RELOC_X86_64_PC32; // treat PLT32 as PC32
            case R_X86_64_GOTPCRELX:   return ASM_RELOC_X86_64_GOTPCRELX;
            default:                    return -1;
        }
    }
    // aarch64
    switch (elf_type) {
        case R_AARCH64_ABS64:            return ASM_RELOC_UNSIGNED;
        case R_AARCH64_CALL26:           return ASM_RELOC_BRANCH26;
        case R_AARCH64_ADR_PREL_PG_HI21: return ASM_RELOC_PAGE21;
        case R_AARCH64_ADD_ABS_LO12_NC:  return ASM_RELOC_PAGEOFF12;
        default:                         return -1;
    }
}

// Map an ELF section header name to an ASM_SEC_* index.
static int section_name_to_idx(const char *name) {
    if (strcmp(name, ".text") == 0) return ASM_SEC_TEXT;
    if (strcmp(name, ".data") == 0) return ASM_SEC_DATA;
    if (strcmp(name, ".bss") == 0) return ASM_SEC_BSS;
    if (strcmp(name, ".rodata") == 0) return ASM_SEC_RODATA;
    return -1;
}

int elf_read(const uint8_t *data, size_t len, ElfObject *out,
             char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));

    if (len < 64) { snprintf(err, errlen, "file too small for ELF header"); return 1; }
    if (memcmp(data, "\x7f" "ELF", 4) != 0) { snprintf(err, errlen, "not an ELF file"); return 1; }
    if (data[4] != ELFCLASS64) { snprintf(err, errlen, "not a 64-bit ELF file"); return 1; }
    if (data[5] != ELFDATA2LSB) { snprintf(err, errlen, "not a little-endian ELF file"); return 1; }

    uint16_t e_type, e_machine, e_shnum, e_shentsize, e_shstrndx;
    uint64_t e_shoff;
    if (read_u16(data, len, 16, &e_type) != 0) goto trunc;
    if (read_u16(data, len, 18, &e_machine) != 0) goto trunc;
    if (read_u64(data, len, 40, &e_shoff) != 0) goto trunc;
    if (read_u16(data, len, 58, &e_shentsize) != 0) goto trunc;
    if (read_u16(data, len, 60, &e_shnum) != 0) goto trunc;
    if (read_u16(data, len, 62, &e_shstrndx) != 0) goto trunc;

    if (e_type != ET_REL) { snprintf(err, errlen, "not a relocatable ELF object"); return 1; }
    if (e_machine != EM_AARCH64 && e_machine != EM_X86_64 && e_machine != EM_RISCV) {
        snprintf(err, errlen, "unsupported ELF machine type %u", e_machine);
        return 1;
    }
    out->machine = e_machine;
    if (e_shentsize != 64) { snprintf(err, errlen, "unexpected section header size"); return 1; }
    if (e_shoff + (uint64_t)e_shnum * 64 > len) goto trunc;
    if (e_shstrndx >= e_shnum) { snprintf(err, errlen, "invalid shstrndx"); return 1; }

    // Read shstrtab section header.
    size_t shstr_sh = (size_t)e_shoff + (size_t)e_shstrndx * 64;
    uint64_t shstr_off, shstr_size;
    if (read_u64(data, len, shstr_sh + 24, &shstr_off) != 0) goto trunc;
    if (read_u64(data, len, shstr_sh + 32, &shstr_size) != 0) goto trunc;
    if (shstr_off + shstr_size > len) goto trunc;
    const uint8_t *shstrtab = data + shstr_off;

    // First pass: find symtab and count sections.
    uint32_t symtab_idx = 0;
    for (uint32_t i = 0; i < e_shnum; i++) {
        size_t sh = (size_t)e_shoff + (size_t)i * 64;
        uint32_t sh_type;
        if (read_u32(data, len, sh + 4, &sh_type) != 0) goto trunc;
        if (sh_type == SHT_SYMTAB) symtab_idx = i;
    }

    // Allocate section array (one per content section we care about).
    out->sections = calloc(e_shnum, sizeof(ElfSection));
    out->nsection = 0;

    // Map from ELF section index to our section index.
    int *sec_map = calloc(e_shnum, sizeof(int));
    for (uint32_t i = 0; i < e_shnum; i++) sec_map[i] = -1;

    // Second pass: extract content sections.
    for (uint32_t i = 0; i < e_shnum; i++) {
        size_t sh = (size_t)e_shoff + (size_t)i * 64;
        uint32_t sh_name, sh_type;
        uint64_t sh_flags, sh_offset, sh_size, sh_entsize;
        uint32_t sh_link;
        if (read_u32(data, len, sh, &sh_name) != 0) goto trunc_free;
        if (read_u32(data, len, sh + 4, &sh_type) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 8, &sh_flags) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 24, &sh_offset) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 32, &sh_size) != 0) goto trunc_free;
        if (read_u32(data, len, sh + 40, &sh_link) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 56, &sh_entsize) != 0) goto trunc_free;

        if (sh_name >= shstr_size) continue;
        const char *name = (const char *)shstrtab + sh_name;
        int idx = section_name_to_idx(name);
        if (idx < 0) continue;

        uint32_t si = out->nsection;
        sec_map[i] = (int)si;
        ElfSection *s = &out->sections[si];
        memset(s, 0, sizeof(*s));
        strncpy(s->name, name, 16);
        s->type = sh_type;
        s->flags = sh_flags;
        s->size = sh_size;
        s->link = sh_link;
        s->entsize = sh_entsize;

        if (sh_type != SHT_NOBITS && sh_size > 0) {
            if (sh_offset + sh_size > len) goto trunc_free;
            s->data = malloc((size_t)sh_size);
            memcpy(s->data, data + sh_offset, (size_t)sh_size);
        }
        out->nsection = si + 1;
    }

    // Third pass: extract relocations.
    for (uint32_t i = 0; i < e_shnum; i++) {
        size_t sh = (size_t)e_shoff + (size_t)i * 64;
        uint32_t sh_type, sh_info;
        uint64_t sh_offset, sh_size;
        if (read_u32(data, len, sh + 4, &sh_type) != 0) goto trunc_free;
        if (read_u32(data, len, sh + 44, &sh_info) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 24, &sh_offset) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 32, &sh_size) != 0) goto trunc_free;

        if (sh_type != SHT_RELA) continue;
        if (sh_info >= e_shnum || sec_map[sh_info] < 0) continue;

        ElfSection *target = &out->sections[sec_map[sh_info]];
        uint32_t nrelocs = (uint32_t)(sh_size / 24);
        target->relocs = calloc(nrelocs, sizeof(ElfReloc));
        target->nreloc = 0;

        for (uint32_t r = 0; r < nrelocs; r++) {
            size_t roff = (size_t)sh_offset + (size_t)r * 24;
            uint64_t r_offset, r_info;
            int64_t r_addend;
            if (read_u64(data, len, roff, &r_offset) != 0) goto trunc_free;
            if (read_u64(data, len, roff + 8, &r_info) != 0) goto trunc_free;
            if (read_i64(data, len, roff + 16, &r_addend) != 0) goto trunc_free;

            uint32_t r_sym = (uint32_t)(r_info >> 32);
            uint32_t r_type = (uint32_t)(r_info & 0xFFFFFFFF);
            int itype = internal_reloc_type(e_machine, r_type);
            if (itype < 0) {
                snprintf(err, errlen, "unsupported relocation type %u", r_type);
                goto trunc_free;
            }
            ElfReloc *rel = &target->relocs[target->nreloc++];
            rel->offset = r_offset;
            rel->sym = (int32_t)(r_sym > 0 ? r_sym - 1 : 0);
            rel->type = itype;
            rel->addend = r_addend;
        }
    }

    // Extract symbols from .symtab.
    if (symtab_idx > 0) {
        size_t sh = (size_t)e_shoff + (size_t)symtab_idx * 64;
        uint64_t sh_offset, sh_size;
        uint32_t sh_link;
        if (read_u64(data, len, sh + 24, &sh_offset) != 0) goto trunc_free;
        if (read_u64(data, len, sh + 32, &sh_size) != 0) goto trunc_free;
        if (read_u32(data, len, sh + 40, &sh_link) != 0) goto trunc_free;

        size_t strtab_sh = (size_t)e_shoff + (size_t)sh_link * 64;
        uint64_t strtab_off, strtab_size;
        if (read_u64(data, len, strtab_sh + 24, &strtab_off) != 0) goto trunc_free;
        if (read_u64(data, len, strtab_sh + 32, &strtab_size) != 0) goto trunc_free;
        if (strtab_off + strtab_size > len) goto trunc_free;
        const uint8_t *sym_strtab = data + strtab_off;

        uint32_t nsyms = (uint32_t)(sh_size / 24);
        uint32_t user_syms = nsyms > 1 ? nsyms - 1 : 0;
        out->symbols = calloc(user_syms > 0 ? user_syms : 1, sizeof(ElfSymbol));
        out->nsymbol = 0;

        for (uint32_t s = 1; s < nsyms; s++) {
            size_t soff = (size_t)sh_offset + (size_t)s * 24;
            uint32_t st_name;
            uint8_t st_info;
            uint16_t st_shndx;
            uint64_t st_value;
            if (read_u32(data, len, soff, &st_name) != 0) goto trunc_free;
            if (soff + 5 > len) goto trunc_free;
            st_info = data[soff + 4];
            if (read_u16(data, len, soff + 6, &st_shndx) != 0) goto trunc_free;
            if (read_u64(data, len, soff + 8, &st_value) != 0) goto trunc_free;

            if (st_name >= strtab_size) continue;
            const char *name = (const char *)sym_strtab + st_name;
            size_t nlen = strlen(name);
            // Keep every entry (even unnamed ones such as the SECTION
            // symbols GCC emits) so that out->symbols[i] corresponds to
            // .symtab entry i+1. Relocations store rel->sym = r_sym - 1
            // and rely on that 1:1 mapping; skipping entries here would
            // misalign every later relocation.
            ElfSymbol *sym = &out->symbols[out->nsymbol];
            sym->name = malloc(nlen + 1);
            memcpy(sym->name, name, nlen + 1);
            sym->binding = (st_info >> 4);
            sym->type = (st_info & 0xf);
            sym->value = st_value;
            sym->common = 0;

            if (st_shndx == 0) sym->section = -1;
            else if (st_shndx == 0xFFF2) { sym->section = -1; sym->common = 1; }
            else if (st_shndx < e_shnum && sec_map[st_shndx] >= 0)
                sym->section = sec_map[st_shndx];
            else sym->section = -1;

            out->nsymbol++;
        }
    }

    free(sec_map);
    return 0;
trunc:
    snprintf(err, errlen, "ELF file truncated");
    return 1;
trunc_free:
    free(sec_map);
    elf_object_free(out);
    return 1;
}

void elf_object_free(ElfObject *o) {
    for (uint32_t i = 0; i < o->nsection; i++) {
        free(o->sections[i].data);
        free(o->sections[i].relocs);
    }
    free(o->sections);
    for (uint32_t i = 0; i < o->nsymbol; i++) free(o->symbols[i].name);
    free(o->symbols);
    memset(o, 0, sizeof(*o));
}
