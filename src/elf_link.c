// M17.3.4: integrated ELF64 executable linker (aarch64 + x86_64).
//
// Combines parsed relocatable objects into a runnable ELF executable
// for aarch64 or x86_64 Linux, without invoking cc/ld:
//   - Two PT_LOAD segments: text (R+X) at 0x400000, data (RW)
//   - PT_DYNAMIC with DT_NEEDED for libc.so.6
//   - GOT for external symbol addresses, PLT stubs for calls
//   - PT_INTERP pointing to the dynamic linker
//   - PT_GNU_STACK (non-executable)
// Output is deterministic: identical inputs yield identical bytes.

#include "../include/elf_link.h"
#include "../include/asm_arm64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000ull
#define TEXT_VM 0x400000ull

typedef struct { uint8_t *bytes; size_t len, cap; } Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        uint8_t *nb = realloc(b->bytes, cap);
        if (!nb) abort();
        b->bytes = nb; b->cap = cap;
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}
static void buf_u8(Buf *b, uint8_t v) { buf_put(b, &v, 1); }
static void buf_le16(Buf *b, uint16_t v) { uint8_t d[2] = {(uint8_t)v,(uint8_t)(v>>8)}; buf_put(b,d,2); }
static void buf_le32(Buf *b, uint32_t v) { uint8_t d[4]; for(int i=0;i<4;i++) d[i]=(uint8_t)(v>>(8*i)); buf_put(b,d,4); }
static void buf_le64(Buf *b, uint64_t v) { uint8_t d[8]; for(int i=0;i<8;i++) d[i]=(uint8_t)(v>>(8*i)); buf_put(b,d,8); }
static void buf_pad(Buf *b, size_t a) { while (b->len % a) buf_u8(b, 0); }
static void buf_zeros(Buf *b, size_t n) { for (size_t i = 0; i < n; i++) buf_u8(b, 0); }

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
static uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// ELF64 header emission.
static void emit_ehdr(Buf *b, uint16_t type, uint16_t machine, uint64_t entry,
                       uint64_t phoff, uint64_t shoff, uint16_t phnum, uint16_t shnum) {
    buf_put(b, "\x7f" "ELF", 4);
    buf_put(b, "\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    buf_le16(b, type);
    buf_le16(b, machine);
    buf_le32(b, EV_CURRENT);
    buf_le64(b, entry);
    buf_le64(b, phoff);
    buf_le64(b, shoff);
    buf_le32(b, 0); // flags
    buf_le16(b, 64); // ehsize
    buf_le16(b, 56); // phentsize
    buf_le16(b, phnum);
    buf_le16(b, 64); // shentsize
    buf_le16(b, shnum);
    buf_le16(b, 0); // shstrndx
}

// ELF64 program header emission.
static void emit_phdr(Buf *b, uint32_t type, uint32_t flags, uint64_t off,
                       uint64_t vaddr, uint64_t filesz, uint64_t memsz,
                       uint64_t align) {
    buf_le32(b, type);
    buf_le32(b, flags);
    buf_le64(b, off);
    buf_le64(b, vaddr);
    buf_le64(b, 0); // paddr
    buf_le64(b, filesz);
    buf_le64(b, memsz);
    buf_le64(b, align);
}

// ELF64 dynamic entry.
static void emit_dyn(Buf *b, uint64_t tag, uint64_t val) {
    buf_le64(b, tag);
    buf_le64(b, val);
}

// ELF64 symbol table entry (for .dynsym).
static void emit_dynsym(Buf *b, uint32_t name, uint8_t info, uint8_t other,
                          uint16_t shndx, uint64_t value, uint64_t size) {
    buf_le32(b, name);
    buf_u8(b, info);
    buf_u8(b, other);
    buf_le16(b, shndx);
    buf_le64(b, value);
    buf_le64(b, size);
}

// ELF64 Rela entry.
static void emit_rela(Buf *b, uint64_t off, uint64_t info, int64_t addend) {
    buf_le64(b, off);
    buf_le64(b, info);
    buf_le64(b, (uint64_t)addend);
}

// Find a content section by name in an ElfObject.
static const ElfSection *find_section(const ElfObject *o, const char *name) {
    for (uint32_t i = 0; i < o->nsection; i++)
        if (strcmp(o->sections[i].name, name) == 0) return &o->sections[i];
    return NULL;
}

// Find a global symbol by name across all objects.
static int find_symbol(const ElfObject *objs, size_t nobj, const char *name,
                        int *obj_idx, int *sym_idx) {
    for (size_t o = 0; o < nobj; o++) {
        for (uint32_t s = 0; s < objs[o].nsymbol; s++) {
            const ElfSymbol *sym = &objs[o].symbols[s];
            if (sym->binding == STB_GLOBAL && sym->section >= 0 &&
                strcmp(sym->name, name) == 0) {
                *obj_idx = (int)o;
                *sym_idx = (int)s;
                return 1;
            }
        }
    }
    return 0;
}

// Check if a symbol is defined in any object.
static int symbol_defined(const ElfObject *objs, size_t nobj, const char *name) {
    int oi, si;
    return find_symbol(objs, nobj, name, &oi, &si);
}

// Collect all undefined global symbols that need GOT/PLT entries.
typedef struct { char *name; int got_idx; int plt_idx; } ExtSym;

static int collect_externals(const ElfObject *objs, size_t nobj,
                              ExtSym **out_ext, size_t *out_next) {
    ExtSym *ext = NULL;
    size_t next = 0, cap = 0;

    for (size_t o = 0; o < nobj; o++) {
        for (uint32_t s = 0; s < objs[o].nsymbol; s++) {
            const ElfSymbol *sym = &objs[o].symbols[s];
            if (sym->binding != STB_GLOBAL || sym->section >= 0) continue;
            if (sym->common) continue;
            // Check if already collected.
            int found = 0;
            for (size_t e = 0; e < next; e++)
                if (strcmp(ext[e].name, sym->name) == 0) { found = 1; break; }
            if (found) continue;
            // Check if defined in any object.
            if (symbol_defined(objs, nobj, sym->name)) continue;
            // New external symbol.
            if (next >= cap) { cap = cap ? cap * 2 : 16; ext = realloc(ext, cap * sizeof(ExtSym)); }
            ext[next].name = malloc(strlen(sym->name) + 1);
            strcpy(ext[next].name, sym->name);
            ext[next].got_idx = -1;
            ext[next].plt_idx = -1;
            next++;
        }
    }
    *out_ext = ext;
    *out_next = next;
    return 0;
}

int link_elf_exec(const ElfObject *objs, size_t nobj, const char *entry,
                  uint8_t **out, size_t *out_len, char *err, size_t errlen) {
    *out = NULL;
    *out_len = 0;

    // Detect machine type from first object.
    uint16_t machine = nobj > 0 ? objs[0].machine : EM_AARCH64;
    int is_x86_64 = (machine == EM_X86_64);
    int is_riscv64 = (machine == EM_RISCV);

    // Find entry symbol.
    int entry_oi, entry_si;
    if (!find_symbol(objs, nobj, entry, &entry_oi, &entry_si)) {
        snprintf(err, errlen, "unresolved entry symbol '%s'", entry);
        return 1;
    }

    // Collect external (undefined) symbols.
    ExtSym *ext = NULL;
    size_t next = 0;
    collect_externals(objs, nobj, &ext, &next);

    // Assign GOT and PLT indices. Each external gets a GOT entry and a PLT slot.
    for (size_t i = 0; i < next; i++) {
        ext[i].got_idx = (int)i;
        ext[i].plt_idx = (int)i;
    }

    // Interpreter path depends on architecture.
    const char *interp;
    if (is_x86_64) interp = "/lib64/ld-linux-x86-64.so.2";
    else if (is_riscv64) interp = "/lib/ld-linux-riscv64-lp64d.so.1";
    else interp = "/lib/ld-linux-aarch64.so.1";
    size_t interp_len = strlen(interp) + 1;

    // --- Compute section sizes ---
    // Combine .text from all objects.
    size_t total_text = 0;
    size_t total_rodata = 0;
    size_t total_data = 0;
    size_t total_bss = 0;

    // Per-object text offsets within the combined text section.
    size_t *text_off = calloc(nobj, sizeof(size_t));
    size_t *rodata_off = calloc(nobj, sizeof(size_t));
    size_t *data_off = calloc(nobj, sizeof(size_t));

    for (size_t o = 0; o < nobj; o++) {
        const ElfSection *t = find_section(&objs[o], ".text");
        text_off[o] = total_text;
        if (t) total_text += t->size;

        const ElfSection *r = find_section(&objs[o], ".rodata");
        rodata_off[o] = total_rodata;
        if (r) total_rodata += r->size;

        const ElfSection *d = find_section(&objs[o], ".data");
        data_off[o] = total_data;
        if (d) total_data += d->size;

        const ElfSection *b = find_section(&objs[o], ".bss");
        if (b) total_bss += b->size;
    }

    // PLT entry size depends on architecture.
    // aarch64: 16 bytes (adrp, ldr, add, br)
    // x86_64: 16 bytes (jmp *GOT(%rip), nop padding)
    // riscv64: 16 bytes (auipc t0, ld t0, jalr t0, nop pad)
    size_t plt_entry_size = 16;
    size_t plt_size = next * plt_entry_size;
    // GOT: each entry is 8 bytes.
    size_t got_size = next * 8;

    // Dynamic string table: \0 + "libc.so.6\0" + external names.
    size_t dynstr_size = 1; // leading \0
    size_t libc_name_off = dynstr_size;
    dynstr_size += strlen("libc.so.6") + 1;
    size_t *ext_name_off = calloc(next > 0 ? next : 1, sizeof(size_t));
    for (size_t i = 0; i < next; i++) {
        ext_name_off[i] = dynstr_size;
        dynstr_size += strlen(ext[i].name) + 1;
    }

    // .dynsym: null entry + one per external.
    size_t dynsym_size = 24 * (1 + next);

    // .rela.plt: one JUMP_SLOT per PLT entry.
    uint32_t jump_slot_type;
    if (is_x86_64) jump_slot_type = R_X86_64_64;
    else if (is_riscv64) jump_slot_type = R_RISCV_JUMP_SLOT;
    else jump_slot_type = R_AARCH64_JUMP_SLOT;
    // Note: for x86_64 we use R_X86_64_64 for GOT entries (ld.so fills them).
    // Actually, the standard relocation for PLT GOT entries is R_X86_64_JUMP_SLOT (256)
    // but that's a dynamic relocation. For simplicity we use R_X86_64_64.
    size_t rela_plt_size = 24 * next;

    // .dynamic entries (each 16 bytes).
    int ndyn = 0;
    // Count: DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT, DT_PLTGOT,
    //        DT_JMPREL, DT_PLTRELSZ, DT_PLTREL, DT_RELASZ, DT_RELAENT,
    //        DT_NEEDED, DT_FLAGS, DT_FLAGS_1, DT_NULL
    ndyn = 14;
    size_t dynamic_size = 16 * (size_t)ndyn;

    // --- Layout ---
    // ELF header (64) + program headers.
    int phnum = 6; // PT_LOAD text, PT_LOAD data, PT_DYNAMIC, PT_INTERP, PT_GNU_STACK, PT_PHDR
    size_t ehdr_size = 64;
    size_t phdr_size = 56 * (size_t)phnum;
    size_t seg_start = ehdr_size + phdr_size;

    // .interp (in text segment).
    size_t interp_off = seg_start;

    // .text (combined from objects).
    size_t text_file_off = align_up(interp_off + interp_len, 16);

    // .plt (after text).
    size_t plt_file_off = align_up(text_file_off + total_text, 16);

    // .rodata (combined, in text segment).
    size_t rodata_file_off = align_up(plt_file_off + plt_size, 8);

    // End of text segment file content.
    size_t text_seg_filesz = align_up(rodata_file_off + total_rodata, PAGE);

    // Data segment starts at next page.
    size_t data_seg_vm = align_up(TEXT_VM + text_seg_filesz, PAGE);

    // .data (combined).
    size_t data_file_off = text_seg_filesz; // offset in file
    size_t data_seg_off = 0; // offset within data segment

    // .got (in data segment).
    size_t got_seg_off = align_up(data_seg_off + total_data, 8);

    // .dynsym.
    size_t dynsym_seg_off = align_up(got_seg_off + got_size, 8);

    // .dynstr.
    size_t dynstr_seg_off = dynsym_seg_off + dynsym_size;

    // .rela.plt.
    size_t rela_plt_seg_off = align_up(dynstr_seg_off + dynstr_size, 8);

    // .dynamic.
    size_t dynamic_seg_off = align_up(rela_plt_seg_off + rela_plt_size, 8);

    // .bss (after dynamic, in data segment).
    size_t bss_seg_off = align_up(dynamic_seg_off + dynamic_size, 8);

    size_t data_seg_filesz = dynamic_seg_off + dynamic_size;
    size_t data_seg_memsz = align_up(bss_seg_off + total_bss, 8);

    // Virtual addresses.
    uint64_t text_vm = TEXT_VM;
    uint64_t interp_vm = text_vm + interp_off;
    uint64_t text_vm_off = text_vm + text_file_off;
    uint64_t plt_vm = text_vm + plt_file_off;
    uint64_t rodata_vm = text_vm + rodata_file_off;
    uint64_t data_vm = data_seg_vm;
    uint64_t got_vm = data_vm + got_seg_off;
    (void)rodata_vm;
    uint64_t dynsym_vm = data_vm + dynsym_seg_off;
    uint64_t dynstr_vm = data_vm + dynstr_seg_off;
    uint64_t rela_plt_vm = data_vm + rela_plt_seg_off;
    uint64_t dynamic_vm = data_vm + dynamic_seg_off;

    // Section headers (0 for executable).
    uint16_t shnum = 0;

    // Entry point: the entry symbol's virtual address.
    const ElfSymbol *entry_sym = &objs[entry_oi].symbols[entry_si];
    const ElfSection *entry_text = find_section(&objs[entry_oi], ".text");
    uint64_t entry_vm = 0;
    if (entry_text && strcmp(entry_text->name, ".text") == 0) {
        entry_vm = text_vm_off + text_off[entry_oi] + entry_sym->value;
    }

    // --- Build the output ---
    Buf b = { 0 };

    // ELF header (placeholder, fill in later).
    emit_ehdr(&b, ET_EXEC, machine, entry_vm, ehdr_size, 0, (uint16_t)phnum, shnum);

    // Program headers.
    // PT_PHDR
    emit_phdr(&b, PT_PHDR, 4, ehdr_size, TEXT_VM + ehdr_size,
              phdr_size, phdr_size, 8);
    // PT_INTERP
    emit_phdr(&b, PT_INTERP, 4, interp_off, interp_vm, interp_len, interp_len, 1);
    // PT_LOAD text (R+X)
    emit_phdr(&b, PT_LOAD, 5, 0, TEXT_VM, text_seg_filesz, text_seg_filesz, PAGE);
    // PT_LOAD data (RW)
    emit_phdr(&b, PT_LOAD, 6, data_seg_vm - data_vm + data_file_off,
              data_vm, data_seg_filesz, data_seg_memsz, PAGE);
    // PT_DYNAMIC
    emit_phdr(&b, PT_DYNAMIC, 6, data_file_off + dynamic_seg_off,
              dynamic_vm, dynamic_size, dynamic_size, 8);
    // PT_GNU_STACK (non-executable)
    emit_phdr(&b, PT_GNU_STACK, 6, 0, 0, 0, 0, 16);

    // .interp
    buf_pad(&b, 8);
    buf_put(&b, interp, interp_len);

    // .text (combined from all objects).
    buf_pad(&b, 16);
    for (size_t o = 0; o < nobj; o++) {
        const ElfSection *t = find_section(&objs[o], ".text");
        if (t && t->size > 0) buf_put(&b, t->data, (size_t)t->size);
    }

    // .plt: architecture-specific stubs.
    if (is_x86_64) {
        // x86_64 PLT entry: jmp *GOT_ENTRY(%rip) + nop padding to 16 bytes.
        // FF 25 xx xx xx xx  (jmp *[rip+xx], xx = offset to GOT entry from next insn)
        // 0F 1F 44 00 00     (nop dword ptr [rax+rax], 5-byte nop)
        // 0F 1F 44 00 00     (another 5-byte nop)
        // 66 0F 1F 44 00 00  (nop word ptr [rax+rax], 6-byte nop) -- only if needed
        for (size_t i = 0; i < next; i++) {
            uint64_t got_entry_vm = got_vm + (uint64_t)i * 8;
            uint64_t plt_entry_vm = plt_vm + i * plt_entry_size;
            // The jmp is at plt_entry_vm, rip after jmp = plt_entry_vm + 6.
            // Offset to GOT entry from rip = got_entry_vm - (plt_entry_vm + 6).
            int32_t rip_offset = (int32_t)(got_entry_vm - (plt_entry_vm + 6));
            buf_u8(&b, 0xFF); buf_u8(&b, 0x25); // jmp *rip+
            buf_le32(&b, (uint32_t)rip_offset);
            // 10 bytes of nop padding.
            buf_u8(&b, 0x0F); buf_u8(&b, 0x1F); buf_u8(&b, 0x44);
            buf_u8(&b, 0x00); buf_u8(&b, 0x00); // 5-byte nop
            buf_u8(&b, 0x0F); buf_u8(&b, 0x1F); buf_u8(&b, 0x44);
            buf_u8(&b, 0x00); buf_u8(&b, 0x00); // 5-byte nop
        }
    } else if (is_riscv64) {
        // riscv64 PLT entry (4 instructions): auipc t0, %pcrel_hi(GOT);
        // ld t0, %pcrel_lo(t0); jalr t0; nop. Patched below with the
        // resolved auipc/lo12 fields.
        for (size_t i = 0; i < next; i++) {
            uint8_t *e = (uint8_t *)(b.bytes + b.len);
            uint8_t stub[16] = { 0 };
            wr_u32(stub, 0x00000297u);      // auipc t0, 0
            wr_u32(stub + 4, 0x0002b283u);  // ld t0, 0(t0)
            wr_u32(stub + 8, 0x00028067u);  // jalr x0, 0(t0)
            wr_u32(stub + 12, 0x00000013u); // nop
            buf_put(&b, stub, 16);
            (void)e;
        }
    } else {
        // aarch64 PLT entry: adrp x16, GOT_page; ldr x17, [x16, #lo12];
        //                    add x16, x16, #lo12; br x17
        for (size_t i = 0; i < next; i++) {
            uint64_t got_entry_vm = got_vm + (uint64_t)i * 8;
            // We'll patch these with proper adrp/ldr/add/br after layout is final.
            // For now emit NOPs as placeholders.
            for (int j = 0; j < 4; j++) buf_le32(&b, 0xD503201F); // nop
            (void)got_entry_vm;
        }
    }

    // .rodata (combined).
    buf_pad(&b, 8);
    for (size_t o = 0; o < nobj; o++) {
        const ElfSection *r = find_section(&objs[o], ".rodata");
        if (r && r->size > 0) buf_put(&b, r->data, (size_t)r->size);
    }

    // Data segment content.
    buf_pad(&b, PAGE); // pad to page boundary for data segment

    // .data (combined).
    for (size_t o = 0; o < nobj; o++) {
        const ElfSection *d = find_section(&objs[o], ".data");
        if (d && d->size > 0) buf_put(&b, d->data, (size_t)d->size);
    }
    // Pad to GOT alignment.
    buf_pad(&b, 8);

    // .got (zeroed; ld.so fills it via JUMP_SLOT relocations).
    buf_zeros(&b, got_size);
    buf_pad(&b, 8);

    // .dynsym
    // Null entry.
    emit_dynsym(&b, 0, 0, 0, 0, 0, 0);
    // One entry per external symbol.
    for (size_t i = 0; i < next; i++) {
        uint8_t info = (uint8_t)((STB_GLOBAL << 4) | STT_NOTYPE);
        emit_dynsym(&b, (uint32_t)ext_name_off[i], info, 0, 0, 0, 0);
    }
    buf_pad(&b, 8);

    // .dynstr
    buf_u8(&b, 0); // leading NUL
    buf_put(&b, "libc.so.6", 10); // includes trailing NUL
    for (size_t i = 0; i < next; i++)
        buf_put(&b, ext[i].name, strlen(ext[i].name) + 1);
    buf_pad(&b, 8);

    // .rela.plt
    for (size_t i = 0; i < next; i++) {
        uint64_t got_off_vm = got_vm + (uint64_t)i * 8;
        uint64_t r_info = ((uint64_t)(i + 1) << 32) | (uint64_t)jump_slot_type;
        emit_rela(&b, got_off_vm, r_info, 0);
    }

    // .dynamic
    emit_dyn(&b, DT_NEEDED, libc_name_off);
    emit_dyn(&b, DT_STRTAB, dynstr_vm);
    emit_dyn(&b, DT_SYMTAB, dynsym_vm);
    emit_dyn(&b, DT_STRSZ, dynstr_size);
    emit_dyn(&b, DT_SYMENT, 24);
    emit_dyn(&b, DT_PLTGOT, got_vm);
    emit_dyn(&b, DT_JMPREL, rela_plt_vm);
    emit_dyn(&b, DT_PLTRELSZ, rela_plt_size);
    emit_dyn(&b, DT_PLTREL, DT_RELA);
    emit_dyn(&b, DT_RELASZ, rela_plt_size);
    emit_dyn(&b, DT_RELAENT, 24);
    emit_dyn(&b, DT_FLAGS, DF_BIND_NOW);
    emit_dyn(&b, DT_FLAGS_1, DF_1_NOW);
    emit_dyn(&b, DT_NULL, 0);

    // --- Apply relocations ---
    // Now patch the text section bytes in the buffer for internal relocations.
    uint8_t *text_base = b.bytes + text_file_off;
    uint8_t *data_base = b.bytes + data_file_off;

    for (size_t o = 0; o < nobj; o++) {
        const ElfSection *t = find_section(&objs[o], ".text");
        if (!t) continue;

        for (uint32_t r = 0; r < t->nreloc; r++) {
            const ElfReloc *rel = &t->relocs[r];
            uint8_t *loc = text_base + text_off[o] + rel->offset;

            // Resolve the symbol.
            const ElfSymbol *sym = NULL;
            int sym_defined = 0;
            int sym_oi = -1, sym_si = -1;
            if (rel->sym >= 0 && (uint32_t)rel->sym < objs[o].nsymbol) {
                sym = &objs[o].symbols[rel->sym];
                sym_defined = find_symbol(objs, nobj, sym->name, &sym_oi, &sym_si);
            }

            if (is_x86_64) {
                // x86_64 relocation handling.
                uint64_t P = text_vm_off + text_off[o] + rel->offset;
                switch (rel->type) {
                    case ASM_RELOC_X86_64_PC32: {
                        // S + A - P (A is in rel->addend, typically -4).
                        uint64_t S = 0;
                        if (sym_defined) {
                            const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                            S = text_vm_off + text_off[sym_oi] + def->value;
                        } else if (sym) {
                            // External: redirect to PLT entry.
                            int plt_idx = -1;
                            for (size_t e = 0; e < next; e++)
                                if (strcmp(ext[e].name, sym->name) == 0) { plt_idx = (int)e; break; }
                            if (plt_idx >= 0)
                                S = plt_vm + (uint64_t)plt_idx * plt_entry_size;
                        }
                        int32_t result = (int32_t)(S + (uint64_t)rel->addend - P);
                        uint8_t d[4];
                        memcpy(d, &result, 4);
                        memcpy(loc, d, 4);
                        break;
                    }
                    case ASM_RELOC_X86_64_GOTPCRELX: {
                        // GOT entry VA + A - P (A is typically -4).
                        if (sym) {
                            int got_idx = -1;
                            for (size_t e = 0; e < next; e++)
                                if (strcmp(ext[e].name, sym->name) == 0) { got_idx = (int)e; break; }
                            if (got_idx >= 0) {
                                uint64_t got_entry_vm = got_vm + (uint64_t)got_idx * 8;
                                int32_t result = (int32_t)(got_entry_vm + (uint64_t)rel->addend - P);
                                uint8_t d[4];
                                memcpy(d, &result, 4);
                                memcpy(loc, d, 4);
                            }
                        }
                        break;
                    }
                    case ASM_RELOC_X86_64_64: {
                        // Absolute 64-bit: S + A (in data section typically).
                        uint8_t *dloc = loc;
                        uint64_t val = 0;
                        memcpy(&val, dloc, 8);
                        if (sym_defined) {
                            const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                            uint64_t sym_vm = text_vm_off + text_off[sym_oi] + def->value;
                            val += sym_vm;
                        }
                        wr_u64(dloc, val);
                        break;
                    }
                    default:
                        break;
                }
            } else if (is_riscv64) {
                // riscv64 relocation handling.
                //
                // Pseudo-instructions are assembled as an auipc immediately
                // followed by its lo12 instruction (see asm_rv64.c); the
                // auipc carries the R_RISCV_PCREL_HI20 reloc at P and the
                // paired lo12 carries R_RISCV_PCREL_LO12_I/S at P+4, both
                // referencing the same symbol. For an external symbol the
                // target depends on the lo12 instruction: a jalr is a call
                // -> PLT stub; any other instruction (load/store/addi)
                // -> GOT slot, whose contents hold the resolved address.
                uint32_t insn_op = rd_u32(loc) & 0x7F;
                if (rel->type == ASM_RELOC_RISCV_HI20)
                    insn_op = rd_u32(loc + 4) & 0x7F; // paired lo12 instruction
                uint64_t S;
                if (sym_defined) {
                    const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                    S = text_vm_off + text_off[sym_oi] + def->value;
                } else if (sym) {
                    int ext_idx = -1;
                    for (size_t e = 0; e < next; e++)
                        if (strcmp(ext[e].name, sym->name) == 0) { ext_idx = (int)e; break; }
                    if (ext_idx >= 0) {
                        if (insn_op == 0x67) // jalr: call -> PLT stub
                            S = plt_vm + (uint64_t)ext_idx * plt_entry_size;
                        else                 // load/store/addi -> GOT slot
                            S = got_vm + (uint64_t)ext_idx * 8;
                    } else {
                        S = 0;
                    }
                } else {
                    S = 0;
                }
                switch (rel->type) {
                    case ASM_RELOC_RISCV_HI20: {
                        uint64_t P = text_vm_off + text_off[o] + rel->offset;
                        int64_t val = (int64_t)(S + rel->addend - P);
                        int64_t hi = (val + 0x800) >> 12;
                        uint32_t w = rd_u32(loc);
                        w = (w & 0x00000FFFu) | (uint32_t)((hi & 0xFFFFF) << 12);
                        wr_u32(loc, w);
                        break;
                    }
                    case ASM_RELOC_RISCV_LO12_I: {
                        // lo12 refers to the paired auipc at B-4 (adjacency).
                        uint64_t P_auipc = text_vm_off + text_off[o] + rel->offset - 4;
                        int64_t val = (int64_t)(S + rel->addend - P_auipc);
                        uint32_t lo12 = (uint32_t)(val & 0xFFF);
                        uint32_t w = rd_u32(loc);
                        w = (w & ~0xFFF00000u) | (lo12 << 20);
                        wr_u32(loc, w);
                        break;
                    }
                    case ASM_RELOC_RISCV_LO12_S: {
                        uint64_t P_auipc = text_vm_off + text_off[o] + rel->offset - 4;
                        int64_t val = (int64_t)(S + rel->addend - P_auipc);
                        uint32_t lo12 = (uint32_t)(val & 0xFFF);
                        uint32_t w = rd_u32(loc);
                        w = (w & ~0xFE000F80u) | ((lo12 >> 5) << 25) | ((lo12 & 0x1F) << 7);
                        wr_u32(loc, w);
                        break;
                    }
                    case ASM_RELOC_RISCV_64: {
                        // Absolute 64-bit: S + A in data.
                        uint8_t *dloc = data_base + data_off[o] + rel->offset;
                        uint64_t val = 0;
                        memcpy(&val, dloc, 8);
                        if (sym_defined)
                            val += S + (uint64_t)rel->addend;
                        wr_u64(dloc, val);
                        break;
                    }
                    case ASM_RELOC_RISCV_32: {
                        uint8_t *dloc = data_base + data_off[o] + rel->offset;
                        uint32_t val = 0;
                        memcpy(&val, dloc, 4);
                        if (sym_defined)
                            val = (uint32_t)((uint64_t)val + S + (uint64_t)rel->addend);
                        memcpy(dloc, &val, 4);
                        break;
                    }
                    default:
                        break;
                }
            } else {
                uint32_t insn = rd_u32(loc);
                switch (rel->type) {
                    case ASM_RELOC_BRANCH26: {
                        if (sym_defined) {
                            const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                            uint64_t target = text_vm_off + text_off[sym_oi] + def->value;
                            uint64_t pc = text_vm_off + text_off[o] + rel->offset;
                            int64_t delta = (int64_t)(target - pc) / 4;
                            insn |= (uint32_t)delta & 0x03FFFFFFu;
                        } else if (sym) {
                            int plt_idx = -1;
                            for (size_t e = 0; e < next; e++)
                                if (strcmp(ext[e].name, sym->name) == 0) { plt_idx = (int)e; break; }
                            if (plt_idx >= 0) {
                                uint64_t plt_entry_vm = plt_vm + (uint64_t)plt_idx * plt_entry_size;
                                uint64_t pc = text_vm_off + text_off[o] + rel->offset;
                                int64_t delta = (int64_t)(plt_entry_vm - pc) / 4;
                                insn |= (uint32_t)delta & 0x03FFFFFFu;
                            }
                        }
                        wr_u32(loc, insn);
                        break;
                    }
                    case ASM_RELOC_PAGE21: {
                        if (sym && !sym_defined) {
                            int got_idx = -1;
                            for (size_t e = 0; e < next; e++)
                                if (strcmp(ext[e].name, sym->name) == 0) { got_idx = (int)e; break; }
                            if (got_idx >= 0) {
                                uint64_t got_entry_vm = got_vm + (uint64_t)got_idx * 8;
                                uint64_t pc_page = (text_vm_off + text_off[o] + rel->offset) & ~0xFFFu;
                                uint64_t got_page = got_entry_vm & ~0xFFFu;
                                int64_t pages = (int64_t)(got_page - pc_page) / 0x1000;
                                uint32_t immlo = (uint32_t)(pages & 3) << 29;
                                uint32_t immhi = (uint32_t)((pages >> 2) & 0x7FFFF) << 5;
                                insn = (insn & 0x9F00001Fu) | immlo | immhi;
                            }
                        } else if (sym_defined) {
                            const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                            uint64_t sym_vm = text_vm_off + text_off[sym_oi] + def->value;
                            uint64_t pc_page = (text_vm_off + text_off[o] + rel->offset) & ~0xFFFu;
                            uint64_t sym_page = sym_vm & ~0xFFFu;
                            int64_t pages = (int64_t)(sym_page - pc_page) / 0x1000;
                            uint32_t immlo = (uint32_t)(pages & 3) << 29;
                            uint32_t immhi = (uint32_t)((pages >> 2) & 0x7FFFF) << 5;
                            insn = (insn & 0x9F00001Fu) | immlo | immhi;
                        }
                        wr_u32(loc, insn);
                        break;
                    }
                    case ASM_RELOC_PAGEOFF12: {
                        if (sym && !sym_defined) {
                            int got_idx = -1;
                            for (size_t e = 0; e < next; e++)
                                if (strcmp(ext[e].name, sym->name) == 0) { got_idx = (int)e; break; }
                            if (got_idx >= 0) {
                                uint64_t got_entry_vm = got_vm + (uint64_t)got_idx * 8;
                                uint32_t lo12 = (uint32_t)(got_entry_vm & 0xFFF);
                                insn |= (lo12 << 10);
                            }
                        } else if (sym_defined) {
                            const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                            uint64_t sym_vm = text_vm_off + text_off[sym_oi] + def->value;
                            uint32_t lo12 = (uint32_t)(sym_vm & 0xFFF);
                            insn |= (lo12 << 10);
                        }
                        wr_u32(loc, insn);
                        break;
                    }
                    case ASM_RELOC_UNSIGNED: {
                        uint8_t *dloc = data_base + data_off[o] + rel->offset;
                        uint64_t val = 0;
                        memcpy(&val, dloc, 8);
                        if (sym_defined) {
                            const ElfSymbol *def = &objs[sym_oi].symbols[sym_si];
                            uint64_t sym_vm = text_vm_off + text_off[sym_oi] + def->value;
                            val += sym_vm;
                        }
                        wr_u64(dloc, val);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    // Patch PLT entries (aarch64 and riscv64; x86_64 PLT entries are already
    // complete).
    if (is_riscv64) {
        // auipc t0, %pcrel_hi(GOT[i]); ld t0, %pcrel_lo(t0); jalr t0
        uint8_t *plt_base = b.bytes + plt_file_off;
        for (size_t i = 0; i < next; i++) {
            uint8_t *plt_entry = plt_base + i * 16;
            uint64_t got_entry_vm = got_vm + (uint64_t)i * 8;
            uint64_t plt_pc = plt_vm + (uint64_t)i * 16;
            int64_t val = (int64_t)(got_entry_vm - plt_pc);
            int64_t hi = (val + 0x800) >> 12;
            uint32_t lo12 = (uint32_t)(val & 0xFFF);
            uint32_t auipc = 0x00000297u | (uint32_t)((hi & 0xFFFFF) << 12);
            uint32_t ld = (0x0002b283u & ~0xFFF00000u) | (lo12 << 20);
            wr_u32(plt_entry, auipc);
            wr_u32(plt_entry + 4, ld);
            // jalr t0 (and the nop pad) are already in place.
        }
    } else if (!is_x86_64) {
        uint8_t *plt_base = b.bytes + plt_file_off;
        for (size_t i = 0; i < next; i++) {
            uint8_t *plt_entry = plt_base + i * 16;
            uint64_t got_entry_vm = got_vm + (uint64_t)i * 8;
            uint64_t plt_pc = plt_vm + (uint64_t)i * 16;

            // adrp x16, GOT_page
            uint64_t pc_page = plt_pc & ~0xFFFu;
            uint64_t got_page = got_entry_vm & ~0xFFFu;
            int64_t pages = (int64_t)(got_page - pc_page) / 0x1000;
            uint32_t adrp = 0x90000010u;
            uint32_t immlo = (uint32_t)(pages & 3) << 29;
            uint32_t immhi = (uint32_t)((pages >> 2) & 0x7FFFF) << 5;
            adrp |= immlo | immhi;

            // ldr x17, [x16, #lo12]
            uint32_t lo12 = (uint32_t)(got_entry_vm & 0xFFF);
            uint32_t ldr = 0xF9400211u;
            ldr |= ((lo12 / 8) << 10);

            // add x16, x16, #lo12
            uint32_t add = 0x91000210u;
            add |= (lo12 << 10);

            // br x17
            uint32_t br = 0xD61F0220u;

            wr_u32(plt_entry, adrp);
            wr_u32(plt_entry + 4, ldr);
            wr_u32(plt_entry + 8, add);
            wr_u32(plt_entry + 12, br);
        }
    }

    free(ext_name_off);
    for (size_t i = 0; i < next; i++) free(ext[i].name);
    free(ext);
    free(text_off);
    free(rodata_off);
    free(data_off);

    *out = b.bytes;
    *out_len = b.len;
    return 0;
}
