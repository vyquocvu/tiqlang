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

static int symbol_defined(const ElfObject *objs, size_t nobj, const char *name) {
    int oi, si;
    return find_symbol(objs, nobj, name, &oi, &si);
}

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
            int found = 0;
            for (size_t e = 0; e < next; e++)
                if (strcmp(ext[e].name, sym->name) == 0) { found = 1; break; }
            if (found) continue;
            if (symbol_defined(objs, nobj, sym->name)) continue;
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

static uint64_t symbol_vm(const size_t *text_off, const size_t *rodata_off,
                          const size_t *data_off, const size_t *bss_off,
                          int sym_oi, const ElfSymbol *def,
                          uint64_t text_vm_off, uint64_t rodata_vm,
                          uint64_t data_vm, uint64_t bss_seg_off) {
    switch (def->section) {
        case ASM_SEC_TEXT:   return text_vm_off + text_off[sym_oi] + def->value;
        case ASM_SEC_RODATA: return rodata_vm + rodata_off[sym_oi] + def->value;
        case ASM_SEC_DATA:   return data_vm + data_off[sym_oi] + def->value;
        case ASM_SEC_BSS:    return data_vm + bss_seg_off + bss_off[sym_oi] + def->value;
        default:             return 0;
    }
}

int link_elf_exec(const ElfObject *objs, size_t nobj, const char *entry,
                  uint8_t **out, size_t *out_len, char *err, size_t errlen) {
    *out = NULL;
    *out_len = 0;

    uint16_t machine = nobj > 0 ? objs[0].machine : EM_AARCH64;
    int is_x86_64 = (machine == EM_X86_64);
    int is_riscv64 = (machine == EM_RISCV);

    int entry_oi, entry_si;
    if (!find_symbol(objs, nobj, entry, &entry_oi, &entry_si)) {
        snprintf(err, errlen, "unresolved entry symbol '%s'", entry);
        return 1;
    }

    ExtSym *ext = NULL;
    size_t next = 0;
    collect_externals(objs, nobj, &ext, &next);
    for (size_t i = 0; i < next; i++) {
        ext[i].got_idx = (int)i;
        ext[i].plt_idx = (int)i;
    }

    const char *interp;
    if (is_x86_64) interp = "/lib64/ld-linux-x86-64.so.2";
    else if (is_riscv64) interp = "/lib/ld-linux-riscv64-lp64d.so.1";
    else interp = "/lib/ld-linux-aarch64.so.1";
    size_t interp_len = strlen(interp) + 1;

    size_t total_text = 0, total_rodata = 0, total_data = 0, total_bss = 0;
    size_t *text_off = calloc(nobj, sizeof(size_t));
    size_t *rodata_off = calloc(nobj, sizeof(size_t));
    size_t *data_off = calloc(nobj, sizeof(size_t));
    size_t *bss_off = calloc(nobj, sizeof(size_t));
    for (size_t o = 0; o < nobj; o++) {
        const ElfSection *t = find_section(&objs[o], ".text");
        text_off[o] = total_text; if (t) total_text += t->size;
        const ElfSection *r = find_section(&objs[o], ".rodata");
        rodata_off[o] = total_rodata; if (r) total_rodata += r->size;
        const ElfSection *d = find_section(&objs[o], ".data");
        data_off[o] = total_data; if (d) total_data += d->size;
        const ElfSection *bs = find_section(&objs[o], ".bss");
        bss_off[o] = total_bss; if (bs) total_bss += bs->size;
    }

    size_t plt_entry_size = 16;
    size_t plt_size = next * plt_entry_size;
    size_t got_size = next * 8;
    size_t dynstr_size = 1;
    size_t libc_name_off = dynstr_size;
    dynstr_size += strlen("libc.so.6") + 1;
    size_t *ext_name_off = calloc(next > 0 ? next : 1, sizeof(size_t));
    for (size_t i = 0; i < next; i++) { ext_name_off[i] = dynstr_size; dynstr_size += strlen(ext[i].name) + 1; }
    size_t dynsym_size = 24 * (1 + next);
    uint32_t jump_slot_type = is_x86_64 ? R_X86_64_JUMP_SLOT : (is_riscv64 ? R_RISCV_JUMP_SLOT : R_AARCH64_JUMP_SLOT);
    size_t rela_plt_size = 24 * next;
    size_t dynamic_size = 16 * 14u;

    int phnum = 6;
    size_t ehdr_size = 64, phdr_size = 56 * (size_t)phnum;
    size_t seg_start = ehdr_size + phdr_size;
    size_t interp_off = seg_start;
    size_t text_file_off = align_up(interp_off + interp_len, 16);
    size_t plt_file_off = align_up(text_file_off + total_text, 16);
    size_t rodata_file_off = align_up(plt_file_off + plt_size, 8);
    size_t text_seg_filesz = align_up(rodata_file_off + total_rodata, PAGE);
    size_t data_seg_vm = align_up(TEXT_VM + text_seg_filesz, PAGE);
    size_t data_file_off = text_seg_filesz;
    size_t got_seg_off = align_up(total_data, 8);
    size_t dynsym_seg_off = align_up(got_seg_off + got_size, 8);
    size_t dynstr_seg_off = dynsym_seg_off + dynsym_size;
    size_t rela_plt_seg_off = align_up(dynstr_seg_off + dynstr_size, 8);
    size_t dynamic_seg_off = align_up(rela_plt_seg_off + rela_plt_size, 8);
    size_t bss_seg_off = align_up(dynamic_seg_off + dynamic_size, 8);
    size_t data_seg_filesz = dynamic_seg_off + dynamic_size;
    size_t data_seg_memsz = align_up(bss_seg_off + total_bss, 8);

    uint64_t text_vm = TEXT_VM;
    uint64_t interp_vm = text_vm + interp_off;
    uint64_t text_vm_off = text_vm + text_file_off;
    uint64_t plt_vm = text_vm + plt_file_off;
    uint64_t rodata_vm = text_vm + rodata_file_off;
    uint64_t data_vm = data_seg_vm;
    uint64_t got_vm = data_vm + got_seg_off;
    uint64_t dynsym_vm = data_vm + dynsym_seg_off;
    uint64_t dynstr_vm = data_vm + dynstr_seg_off;
    uint64_t rela_plt_vm = data_vm + rela_plt_seg_off;
    uint64_t dynamic_vm = data_vm + dynamic_seg_off;

    const ElfSymbol *entry_sym = &objs[entry_oi].symbols[entry_si];
    const ElfSection *entry_text = find_section(&objs[entry_oi], ".text");
    uint64_t entry_vm = 0;
    if (entry_text && strcmp(entry_text->name, ".text") == 0)
        entry_vm = text_vm_off + text_off[entry_oi] + entry_sym->value;

    Buf b = { 0 };
    emit_ehdr(&b, ET_EXEC, machine, entry_vm, ehdr_size, 0, (uint16_t)phnum, 0);
    emit_phdr(&b, PT_PHDR, 4, ehdr_size, TEXT_VM + ehdr_size, phdr_size, phdr_size, 8);
    emit_phdr(&b, PT_INTERP, 4, interp_off, interp_vm, interp_len, interp_len, 1);
    emit_phdr(&b, PT_LOAD, 5, 0, TEXT_VM, text_seg_filesz, text_seg_filesz, PAGE);
    emit_phdr(&b, PT_LOAD, 6, data_file_off, data_vm, data_seg_filesz, data_seg_memsz, PAGE);
    emit_phdr(&b, PT_DYNAMIC, 6, data_file_off + dynamic_seg_off, dynamic_vm, dynamic_size, dynamic_size, 8);
    emit_phdr(&b, PT_GNU_STACK, 6, 0, 0, 0, 0, 16);

    buf_pad(&b, 8); buf_put(&b, interp, interp_len);
    buf_pad(&b, 16);
    for (size_t o = 0; o < nobj; o++) { const ElfSection *t = find_section(&objs[o], ".text"); if (t && t->size) buf_put(&b, t->data, (size_t)t->size); }
    buf_pad(&b, 16);

    if (is_x86_64) {
        for (size_t i = 0; i < next; i++) {
            uint64_t got_entry_vm = got_vm + i * 8, plt_entry_vm = plt_vm + i * 16;
            int32_t rip_offset = (int32_t)(got_entry_vm - (plt_entry_vm + 6));
            buf_u8(&b, 0xFF); buf_u8(&b, 0x25); buf_le32(&b, (uint32_t)rip_offset);
            for (int k = 0; k < 2; k++) { buf_u8(&b,0x0F); buf_u8(&b,0x1F); buf_u8(&b,0x44); buf_u8(&b,0); buf_u8(&b,0); }
        }
    } else if (is_riscv64) {
        for (size_t i = 0; i < next; i++) { uint8_t stub[16]={0}; wr_u32(stub,0x00000297u); wr_u32(stub+4,0x0002b283u); wr_u32(stub+8,0x00028067u); wr_u32(stub+12,0x00000013u); buf_put(&b,stub,16); }
    } else {
        for (size_t i=0;i<next;i++) for(int j=0;j<4;j++) buf_le32(&b,0xD503201F);
    }

    buf_pad(&b,8);
    for (size_t o=0;o<nobj;o++){ const ElfSection *r=find_section(&objs[o],".rodata"); if(r&&r->size) buf_put(&b,r->data,(size_t)r->size); }
    buf_pad(&b,PAGE);
    for (size_t o=0;o<nobj;o++){ const ElfSection *d=find_section(&objs[o],".data"); if(d&&d->size) buf_put(&b,d->data,(size_t)d->size); }
    buf_pad(&b,8); buf_zeros(&b,got_size); buf_pad(&b,8);
    emit_dynsym(&b,0,0,0,0,0,0);
    for(size_t i=0;i<next;i++) emit_dynsym(&b,(uint32_t)ext_name_off[i],(uint8_t)(STB_GLOBAL<<4),0,0,0,0);
    buf_pad(&b,8); buf_u8(&b,0); buf_put(&b,"libc.so.6",10);
    for(size_t i=0;i<next;i++) buf_put(&b,ext[i].name,strlen(ext[i].name)+1);
    buf_pad(&b,8);
    for(size_t i=0;i<next;i++) emit_rela(&b,got_vm+i*8,((uint64_t)(i+1)<<32)|jump_slot_type,0);
    emit_dyn(&b,DT_NEEDED,libc_name_off); emit_dyn(&b,DT_STRTAB,dynstr_vm); emit_dyn(&b,DT_SYMTAB,dynsym_vm);
    emit_dyn(&b,DT_STRSZ,dynstr_size); emit_dyn(&b,DT_SYMENT,24); emit_dyn(&b,DT_PLTGOT,got_vm);
    emit_dyn(&b,DT_JMPREL,rela_plt_vm); emit_dyn(&b,DT_PLTRELSZ,rela_plt_size); emit_dyn(&b,DT_PLTREL,DT_RELA);
    emit_dyn(&b,DT_RELASZ,rela_plt_size); emit_dyn(&b,DT_RELAENT,24); emit_dyn(&b,DT_FLAGS,DF_BIND_NOW);
    emit_dyn(&b,DT_FLAGS_1,DF_1_NOW); emit_dyn(&b,DT_NULL,0);

    uint8_t *text_base=b.bytes+text_file_off, *data_base=b.bytes+data_file_off;
    for(size_t o=0;o<nobj;o++){
        const ElfSection *t=find_section(&objs[o],".text"); if(!t) continue;
        for(uint32_t r=0;r<t->nreloc;r++){
            const ElfReloc *rel=&t->relocs[r]; uint8_t *loc=text_base+text_off[o]+rel->offset;
            const ElfSymbol *sym=NULL; int sym_defined=0, sym_oi=-1, sym_si=-1;
            if(rel->sym>=0&&(uint32_t)rel->sym<objs[o].nsymbol){ sym=&objs[o].symbols[rel->sym]; if(sym->section>=0){sym_defined=1;sym_oi=(int)o;sym_si=(int)rel->sym;}else sym_defined=find_symbol(objs,nobj,sym->name,&sym_oi,&sym_si); }
            if(is_x86_64){
                uint64_t P=text_vm_off+text_off[o]+rel->offset;
                if(rel->type==ASM_RELOC_X86_64_PC32){ uint64_t S=0; if(sym_defined){const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];S=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);}else if(sym){for(size_t e=0;e<next;e++)if(strcmp(ext[e].name,sym->name)==0){S=plt_vm+e*16;break;}} int32_t v=(int32_t)(S+(uint64_t)rel->addend-P); memcpy(loc,&v,4); }
                else if(rel->type==ASM_RELOC_X86_64_GOTPCRELX&&sym){for(size_t e=0;e<next;e++)if(strcmp(ext[e].name,sym->name)==0){int32_t v=(int32_t)(got_vm+e*8+(uint64_t)rel->addend-P);memcpy(loc,&v,4);break;}}
                else if(rel->type==ASM_RELOC_X86_64_64&&sym_defined){uint64_t v=0;memcpy(&v,loc,8);const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];v+=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);wr_u64(loc,v);}
            } else if(is_riscv64){
                uint32_t insn_op=rd_u32(loc)&0x7F; if(rel->type==ASM_RELOC_RISCV_HI20) insn_op=rd_u32(loc+4)&0x7F;
                uint64_t S=0; if(sym_defined){const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];S=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);}else if(sym){for(size_t e=0;e<next;e++)if(strcmp(ext[e].name,sym->name)==0){S=(insn_op==0x67)?plt_vm+e*16:got_vm+e*8;break;}}
                if(rel->type==ASM_RELOC_RISCV_HI20){uint64_t P=text_vm_off+text_off[o]+rel->offset;int64_t val=(int64_t)(S+rel->addend-P),hi=(val+0x800)>>12;uint32_t w=rd_u32(loc);w=(w&0xFFF)|((uint32_t)(hi&0xFFFFF)<<12);wr_u32(loc,w);}
                else if(rel->type==ASM_RELOC_RISCV_LO12_I){uint64_t P=text_vm_off+text_off[o]+rel->offset-4;uint32_t lo=(uint32_t)((S+rel->addend-P)&0xFFF);uint32_t w=rd_u32(loc);w=(w&~0xFFF00000u)|(lo<<20);wr_u32(loc,w);}
                else if(rel->type==ASM_RELOC_RISCV_LO12_S){uint64_t P=text_vm_off+text_off[o]+rel->offset-4;uint32_t lo=(uint32_t)((S+rel->addend-P)&0xFFF);uint32_t w=rd_u32(loc);w=(w&~0xFE000F80u)|((lo>>5)<<25)|((lo&0x1F)<<7);wr_u32(loc,w);}
                else if(rel->type==ASM_RELOC_RISCV_64){uint8_t *dloc=data_base+data_off[o]+rel->offset;uint64_t v=0;memcpy(&v,dloc,8);if(sym_defined)v+=S+(uint64_t)rel->addend;wr_u64(dloc,v);}
                else if(rel->type==ASM_RELOC_RISCV_32){uint8_t *dloc=data_base+data_off[o]+rel->offset;uint32_t v=0;memcpy(&v,dloc,4);if(sym_defined)v=(uint32_t)((uint64_t)v+S+(uint64_t)rel->addend);memcpy(dloc,&v,4);}
            } else {
                uint32_t insn=rd_u32(loc);
                if(rel->type==ASM_RELOC_BRANCH26){ if(sym_defined){const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];uint64_t target=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);uint64_t pc=text_vm_off+text_off[o]+rel->offset;insn|=(uint32_t)(((int64_t)(target-pc)/4)&0x03FFFFFFu);} else if(sym){for(size_t e=0;e<next;e++)if(strcmp(ext[e].name,sym->name)==0){uint64_t pc=text_vm_off+text_off[o]+rel->offset;insn|=(uint32_t)(((int64_t)(plt_vm+e*16-pc)/4)&0x03FFFFFFu);break;}} wr_u32(loc,insn); }
                else if(rel->type==ASM_RELOC_PAGE21){uint64_t target=0;if(sym_defined){const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];target=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);}else if(sym){for(size_t e=0;e<next;e++)if(strcmp(ext[e].name,sym->name)==0){target=got_vm+e*8;break;}}if(target){uint64_t pc=(text_vm_off+text_off[o]+rel->offset)&~0xFFFu;int64_t pages=(int64_t)((target&~0xFFFu)-pc)/0x1000;insn=(insn&0x9F00001Fu)|((uint32_t)(pages&3)<<29)|((uint32_t)((pages>>2)&0x7FFFF)<<5);}wr_u32(loc,insn);}
                else if(rel->type==ASM_RELOC_PAGEOFF12){uint64_t target=0;if(sym_defined){const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];target=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);}else if(sym){for(size_t e=0;e<next;e++)if(strcmp(ext[e].name,sym->name)==0){target=got_vm+e*8;break;}}if(target)insn|=((uint32_t)(target&0xFFF)<<10);wr_u32(loc,insn);}
                else if(rel->type==ASM_RELOC_UNSIGNED){uint8_t *dloc=data_base+data_off[o]+rel->offset;uint64_t v=0;memcpy(&v,dloc,8);if(sym_defined){const ElfSymbol *def=&objs[sym_oi].symbols[sym_si];v+=symbol_vm(text_off,rodata_off,data_off,bss_off,sym_oi,def,text_vm_off,rodata_vm,data_vm,bss_seg_off);}wr_u64(dloc,v);}
            }
        }
    }

    if(is_riscv64){uint8_t *plt_base=b.bytes+plt_file_off;for(size_t i=0;i<next;i++){uint8_t *e=plt_base+i*16;uint64_t val=(got_vm+i*8)-(plt_vm+i*16);int64_t hi=((int64_t)val+0x800)>>12;uint32_t lo=(uint32_t)(val&0xFFF);wr_u32(e,0x00000297u|((uint32_t)(hi&0xFFFFF)<<12));wr_u32(e+4,(0x0002b283u&~0xFFF00000u)|(lo<<20));}}
    else if(!is_x86_64){uint8_t *plt_base=b.bytes+plt_file_off;for(size_t i=0;i<next;i++){uint8_t *e=plt_base+i*16;uint64_t g=got_vm+i*8,p=plt_vm+i*16;int64_t pages=(int64_t)((g&~0xFFFu)-(p&~0xFFFu))/0x1000;uint32_t lo=(uint32_t)(g&0xFFF);wr_u32(e,0x90000010u|((uint32_t)(pages&3)<<29)|((uint32_t)((pages>>2)&0x7FFFF)<<5));wr_u32(e+4,0xF9400211u|((lo/8)<<10));wr_u32(e+8,0x91000210u|(lo<<10));wr_u32(e+12,0xD61F0220u);}}

    free(ext_name_off);
    for(size_t i=0;i<next;i++) free(ext[i].name);
    free(ext);
    free(text_off);
    free(rodata_off);
    free(data_off);
    free(bss_off);

    *out=b.bytes;
    *out_len=b.len;
    return 0;
}
