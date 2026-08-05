// M17.3.2: integrated Mach-O arm64 executable linker.
//
// Combines parsed relocatable objects (see macho_read.c) into a
// runnable MH_EXECUTE for arm64 macOS, without invoking cc/ld:
//   - __PAGEZERO, __TEXT (__text + __stubs + __stub_helper +
//     __cstring), __DATA (__data + __common zerofill +
//     __la_symbol_ptr + __got), __LINKEDIT
//   - classic dyld bind opcodes: lazy binding for libSystem calls via
//     __stubs/__la_symbol_ptr/__stub_helper, non-lazy binding of
//     __got[0] to dyld_stub_binder and __got slots for
//     GOT_LOAD_PAGE21/PAGEOFF12 references
//   - LC_MAIN at the requested entry symbol, LC_LOAD_DYLIB libSystem,
//     LC_LOAD_DYLINKER (required by the kernel for main executables)
//   - self-generated ad-hoc code signature (SHA-256 CodeDirectory,
//     16K pages) so the binary runs without host codesign
// The executable is non-PIE, so absolute addresses need no rebase
// stream. Output is deterministic: identical inputs yield identical
// bytes (no timestamps, UUID derived from the input content).

#include "../include/macho_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x4000ull
#define TEXT_VM 0x100000000ull
#define MH_MAGIC_64 0xFEEDFACFu
#define CPU_ARM64 0x0100000Cu
#define MH_EXECUTE 2u
#define MH_NOUNDEFS 0x1u
#define MH_DYLDLINK 0x4u
#define MH_TWOLEVEL 0x80u
// Modern macOS ASP rejects non-PIE main executables; our codegen is fully
// position-independent (adrp/add + GOT), so always emit a PIE image.
#define MH_PIE 0x200000u
#define LC_SEGMENT_64 0x19u
#define LC_SYMTAB 0x2u
#define LC_DYSYMTAB 0xBu
#define LC_LOAD_DYLIB 0xCu
#define LC_LOAD_DYLINKER 0xEu
#define LC_UUID 0x1Bu
#define LC_CODE_SIGNATURE 0x1Du
#define LC_MAIN 0x80000028u
#define LC_DYLD_INFO_ONLY 0x80000022u
#define LC_BUILD_VERSION 0x32u

// dyld classic bind opcodes.
#define BIND_OPCODE_DONE 0x00u
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM 0x10u
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40u
#define BIND_OPCODE_SET_TYPE_IMM 0x50u
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70u
#define BIND_OPCODE_DO_BIND 0x90u
#define BIND_TYPE_POINTER 1u

typedef struct {
    uint8_t *bytes;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        uint8_t *nb = realloc(b->bytes, cap);
        if (!nb) abort();
        b->bytes = nb;
        b->cap = cap;
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}
static void buf_u8(Buf *b, uint8_t v) { buf_put(b, &v, 1); }
static void buf_u16(Buf *b, uint16_t v) { buf_put(b, &v, 2); }
static void buf_u32(Buf *b, uint32_t v) { buf_put(b, &v, 4); }
static void buf_u64(Buf *b, uint64_t v) { buf_put(b, &v, 8); }
static void buf_pad(Buf *b, size_t a) { while (b->len % a) buf_u8(b, 0); }
static void buf_name(Buf *b, const char *s) {
    char f[16];
    memset(f, 0, sizeof(f));
    strncpy(f, s, 15);
    buf_put(b, f, 16);
}
static void buf_uleb(Buf *b, uint64_t v) {
    do {
        uint8_t x = v & 0x7F;
        v >>= 7;
        if (v) x |= 0x80;
        buf_u8(b, x);
    } while (v);
}

static uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// --- SHA-256 (FIPS 180-4) for the ad-hoc code signature -------------------

typedef struct {
    uint32_t h[8];
    uint64_t nbytes;
    uint8_t block[64];
    size_t blocklen;
} Sha256;

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_init(Sha256 *s) {
    static const uint32_t iv[8] = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
    };
    memcpy(s->h, iv, sizeof(iv));
    s->nbytes = 0;
    s->blocklen = 0;
}

static void sha256_block(Sha256 *s, const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428A2F98u,0x71374491u,0xB5C0FBCFu,0xE9B5DBA5u,0x3956C25Bu,0x59F111F1u,0x923F82A4u,0xAB1C5ED5u,
        0xD807AA98u,0x12835B01u,0x243185BEu,0x550C7DC3u,0x72BE5D74u,0x80DEB1FEu,0x9BDC06A7u,0xC19BF174u,
        0xE49B69C1u,0xEFBE4786u,0x0FC19DC6u,0x240CA1CCu,0x2DE92C6Fu,0x4A7484AAu,0x5CB0A9DCu,0x76F988DAu,
        0x983E5152u,0xA831C66Du,0xB00327C8u,0xBF597FC7u,0xC6E00BF3u,0xD5A79147u,0x06CA6351u,0x14292967u,
        0x27B70A85u,0x2E1B2138u,0x4D2C6DFCu,0x53380D13u,0x650A7354u,0x766A0ABBu,0x81C2C92Eu,0x92722C85u,
        0xA2BFE8A1u,0xA81A664Bu,0xC24B8B70u,0xC76C51A3u,0xD192E819u,0xD6990624u,0xF40E3585u,0x106AA070u,
        0x19A4C116u,0x1E376C08u,0x2748774Cu,0x34B0BCB5u,0x391C0CB3u,0x4ED8AA4Au,0x5B9CCA4Fu,0x682E6FF3u,
        0x748F82EEu,0x78A5636Fu,0x84C87814u,0x8CC70208u,0x90BEFFFAu,0xA4506CEBu,0xBEF9A3F7u,0xC67178F2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0], b=s->h[1], c=s->h[2], d=s->h[3];
    uint32_t e=s->h[4], f=s->h[5], g=s->h[6], h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha256_update(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->nbytes += n;
    if (s->blocklen) {
        size_t take = 64 - s->blocklen;
        if (take > n) take = n;
        memcpy(s->block + s->blocklen, p, take);
        s->blocklen += take;
        p += take;
        n -= take;
        if (s->blocklen == 64) {
            sha256_block(s, s->block);
            s->blocklen = 0;
        }
    }
    while (n >= 64) {
        sha256_block(s, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(s->block, p, n);
        s->blocklen = n;
    }
}

static void sha256_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->nbytes * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->blocklen != 56) sha256_update(s, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i*8));
    // Hash the length bytes directly into the pending block to avoid
    // re-counting them in nbytes (final digest reads h[] only).
    memcpy(s->block + 56, lenb, 8);
    sha256_block(s, s->block);
    s->blocklen = 0;
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

static void sha256(const void *data, size_t n, uint8_t out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}

// --- arm64 instruction encoders (fixed up at link time) -------------------

static uint32_t enc_adrp(uint64_t pc, uint64_t target, unsigned rd) {
    int64_t imm = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    uint32_t immlo = (uint32_t)(imm & 3);
    uint32_t immhi = (uint32_t)((imm >> 2) & ((1 << 19) - 1));
    return 0x90000000u | (immlo << 29) | (immhi << 5) | rd;
}
static uint32_t enc_ldr_imm64(unsigned rt, unsigned rn, uint32_t off) {
    return 0xF9400000u | (((off / 8) & 0xFFFu) << 10) | (rn << 5) | rt;
}
static uint32_t enc_b(uint64_t pc, uint64_t target) {
    int64_t imm = (int64_t)(target - pc) / 4;
    return 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu);
}
static uint32_t enc_movz_w(unsigned rd, uint32_t imm16) {
    return 0x52800000u | ((imm16 & 0xFFFFu) << 5) | rd;
}

// --- libSystem export whitelist --------------------------------------------
// Undefined symbols are bound lazily from libSystem. Only symbols in
// this list may resolve there; anything else fails closed. The list is
// exactly what the QBE runtime (src/runtime_qbe.c) imports, plus the
// dyld machinery symbol.
static const char *libsystem_exports[] = {
    "_dyld_stub_binder",
    "___stderrp",
    "_clock_gettime",
    "_exit",
    "_fwrite",
    "_malloc",
    "_memcpy",
    "_printf",
    "_puts",
    "_snprintf",
    "_strcmp",
    "_strlen",
};

static int is_libsystem_export(const char *name) {
    for (size_t i = 0; i < sizeof(libsystem_exports) / sizeof(libsystem_exports[0]); i++)
        if (strcmp(name, libsystem_exports[i]) == 0) return 1;
    return 0;
}

// --- resolved-symbol table --------------------------------------------------

enum { SYM_DEFINED, SYM_EXTERN, SYM_COMMON };

typedef struct {
    const char *name;   // points into input object memory
    uint64_t addr;      // final VM address (SYM_DEFINED), size (SYM_COMMON)
    int kind;
    int ext;            // index into extern table (SYM_EXTERN)
} ResolvedSym;

typedef struct {
    ResolvedSym *v;
    size_t n, cap;
} SymTab;

static ResolvedSym *symtab_add(SymTab *t, const char *name, int kind, uint64_t addr) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        ResolvedSym *nv = realloc(t->v, t->cap * sizeof(ResolvedSym));
        if (!nv) abort();
        t->v = nv;
    }
    ResolvedSym *s = &t->v[t->n++];
    s->name = name;
    s->kind = kind;
    s->addr = addr;
    s->ext = -1;
    return s;
}

static ResolvedSym *symtab_find(SymTab *t, const char *name) {
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0) return &t->v[i];
    return NULL;
}

// --- extern (libSystem) symbol table ----------------------------------------

typedef struct {
    char name[64];
    uint64_t stub_vm, la_ptr_vm;
    uint64_t la_ptr_segrel; // offset within __DATA segment
    uint32_t lazy_off;      // byte offset of this entry in the lazy stream
} ExternSym;

static int fail(char *err, size_t errlen, const char *fmt, const char *arg) {
    snprintf(err, errlen, fmt, arg);
    return 1;
}

typedef struct { uint32_t off; const char *name; } StrX;
typedef struct { Buf *strtab; StrX *v; size_t n, cap; } StrTab;

static uint32_t strtb_intern(StrTab *t, const char *name) {
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0) return t->v[i].off;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        StrX *nv = realloc(t->v, t->cap * sizeof(StrX));
        if (!nv) abort();
        t->v = nv;
    }
    uint32_t off = (uint32_t)t->strtab->len;
    buf_put(t->strtab, name, strlen(name) + 1);
    t->v[t->n].off = off;
    t->v[t->n].name = name;
    t->n++;
    return off;
}

// --- section placement ------------------------------------------------------

typedef struct {
    const MachOObject *obj;
    uint32_t sect;          // section index within obj
    uint64_t base_vm;       // assigned VM address in the output
    int cls;                // 0 text, 1 cstring, 2 data, 3 common, -1 discard
} Placement;

static int section_class(const MachOSection *s) {
    if (strcmp(s->sectname, "__text") == 0) return 0;
    if (strcmp(s->sectname, "__cstring") == 0 || strcmp(s->sectname, "__const") == 0) return 1;
    if (strcmp(s->sectname, "__data") == 0) return 2;
    if (strcmp(s->sectname, "__bss") == 0 || strcmp(s->sectname, "__common") == 0) return 3;
    if (strcmp(s->sectname, "__compact_unwind") == 0) return -1;
    return -2; // unknown: fail closed
}

int link_macho_exec(const MachOObject *objs, size_t nobj, const char *entry,
                    uint8_t **out, size_t *out_len, char *err, size_t errlen) {
    *out = NULL;
    *out_len = 0;
    if (nobj == 0) return fail(err, errlen, "link_macho: no input objects", NULL);

    // ---- pass 1: classify sections, build the symbol table ----
    Placement *pl = calloc(1, sizeof(Placement));
    size_t npl = 0, plcap = 1;
    SymTab syms = {0};

    for (size_t o = 0; o < nobj; o++) {
        const MachOObject *obj = &objs[o];
        for (uint32_t i = 0; i < obj->nsection; i++) {
            int cls = section_class(&obj->sections[i]);
            if (cls == -2)
                return fail(err, errlen, "link_macho: unsupported section '%s'",
                            obj->sections[i].sectname);
            if (npl == plcap) {
                plcap *= 2;
                Placement *np = realloc(pl, plcap * sizeof(Placement));
                if (!np) abort();
                pl = np;
            }
            pl[npl].obj = obj;
            pl[npl].sect = i;
            pl[npl].cls = cls;
            pl[npl].base_vm = 0;
            npl++;
        }
        for (uint32_t i = 0; i < obj->nsymbol; i++) {
            const MachOSymbol *s = &obj->symbols[i];
            if (!s->global) continue;
            // clang emits common symbols as N_SECT in the __common section
            // with n_value = size (see build/runtime_qbe.o); treat globals
            // in zerofill-class sections as common allocations too.
            int is_common = s->common ||
                            (s->section >= 0 &&
                             section_class(&obj->sections[s->section]) == 3);
            if (is_common) {
                if (!symtab_find(&syms, s->name))
                    symtab_add(&syms, s->name, SYM_COMMON, s->value);
                else {
                    ResolvedSym *r = symtab_find(&syms, s->name);
                    if (s->value > r->addr) r->addr = s->value; // common: max size
                }
                continue;
            }
            if (s->section >= 0) {
                ResolvedSym *prev = symtab_find(&syms, s->name);
                if (prev && prev->kind == SYM_DEFINED)
                    return fail(err, errlen, "link_macho: duplicate definition of '%s'", s->name);
                if (prev && prev->kind == SYM_COMMON)
                    return fail(err, errlen, "link_macho: '%s' is both common and defined", s->name);
                if (prev) {           // undefined reference seen earlier
                    prev->kind = SYM_DEFINED;
                    prev->addr = (uint64_t)s->value;
                } else {
                    symtab_add(&syms, s->name, SYM_DEFINED, (uint64_t)s->value);
                }
            } else {
                if (!symtab_find(&syms, s->name))
                    symtab_add(&syms, s->name, SYM_EXTERN, 0);
            }
        }
    }

    // Map each object section to its placement index for reloc resolution.
    // (pl order == per-object section order, so index = running offset.)

    // Entry symbol must be defined.
    ResolvedSym *ent = symtab_find(&syms, entry);
    if (!ent || ent->kind != SYM_DEFINED)
        return fail(err, errlen, "link_macho: entry symbol '%s' is not defined", entry);

    // ---- pass 2: resolve externs ----
    ExternSym *ext = NULL;
    size_t next = 0, extcap = 0;
    for (size_t i = 0; i < syms.n; i++) {
        ResolvedSym *s = &syms.v[i];
        if (s->kind != SYM_EXTERN) continue;
        if (!is_libsystem_export(s->name))
            return fail(err, errlen, "link_macho: undefined symbol '%s'", s->name);
        if (strcmp(s->name, "_dyld_stub_binder") == 0) continue; // bound non-lazy only
        if (next == extcap) {
            extcap = extcap ? extcap * 2 : 8;
            ExternSym *ne = realloc(ext, extcap * sizeof(ExternSym));
            if (!ne) abort();
            ext = ne;
        }
        ExternSym *e = &ext[next];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, s->name, sizeof(e->name) - 1);
        s->ext = (int)next;
        next++;
    }

    // ---- pass 3: assign VM addresses ----
    // __TEXT segment layout: header+cmds (page 0), then __text, __stubs,
    // __stub_helper, __cstring.
    uint64_t text_vm = TEXT_VM + PAGE; // __text starts on page 1
    uint64_t vm = text_vm;
    // input __text sections in object order
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls != 0) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        vm = align_up(vm, 4);
        pl[i].base_vm = vm;
        vm += s->size;
    }
    uint64_t stubs_vm = align_up(vm, 4);
    uint64_t stubs_size = next * 12;
    uint64_t helper_vm = stubs_vm + stubs_size;
    uint64_t helper_size = next ? 12 + next * 8 : 0;
    vm = helper_vm + helper_size;
    uint64_t cstr_vm = 0, cstr_size = 0;
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls != 1) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        if (!cstr_vm) cstr_vm = align_up(vm, 8);
        pl[i].base_vm = cstr_vm + cstr_size;
        cstr_size += s->size;
    }
    uint64_t text_seg_end = cstr_vm ? cstr_vm + cstr_size : vm;
    uint64_t text_filesize = align_up((text_seg_end - TEXT_VM), PAGE);

    // __DATA segment: __data, __common (zerofill), __la_symbol_ptr, __got
    uint64_t data_vm = TEXT_VM + text_filesize;
    uint64_t dvm = data_vm;
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls != 2) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        dvm = align_up(dvm, 8);
        pl[i].base_vm = dvm;
        dvm += s->size;
    }
    uint64_t common_vm = align_up(dvm, 8);
    uint64_t common_size = 0;
    for (size_t i = 0; i < syms.n; i++) {
        ResolvedSym *s = &syms.v[i];
        if (s->kind != SYM_COMMON) continue;
        uint64_t size = s->addr; // temporarily holds size
        common_size = align_up(common_size, 8);
        s->addr = common_vm + common_size;
        common_size += size;
    }
    uint64_t la_ptr_vm = align_up(common_vm + common_size, 8);
    for (size_t i = 0; i < next; i++) {
        ext[i].la_ptr_vm = la_ptr_vm + i * 8;
        ext[i].la_ptr_segrel = (ext[i].la_ptr_vm - data_vm);
    }

    // Lazy bind stream (built early: helper movz immediates in pass 6 need
    // each entry's byte offset within this stream).
    Buf lazy = {0};
    for (size_t i = 0; i < next; i++) {
        if (i == 0) buf_u8(&lazy, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1);
        ext[i].lazy_off = (uint32_t)lazy.len;
        buf_u8(&lazy, BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
        buf_put(&lazy, ext[i].name, strlen(ext[i].name) + 1);
        buf_u8(&lazy, BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 2);
        buf_uleb(&lazy, ext[i].la_ptr_segrel);
        buf_u8(&lazy, BIND_OPCODE_DO_BIND);
    }
    buf_u8(&lazy, BIND_OPCODE_DONE);
    for (size_t i = 0; i < next; i++)
        if (ext[i].lazy_off > 0xFFFFu)
            return fail(err, errlen,
                        "link_macho: lazy bind stream too large for movz w17", NULL);
    uint64_t got_vm = la_ptr_vm + next * 8;
    // __got[0] = dyld_stub_binder; then one slot per GOT reference.
    // GOT references are collected during reloc pass; reserve lazily via a
    // growable list keyed by symbol name.
    (void)got_vm; // assigned below after GOT count is known

    // ---- pass 4: copy section contents, collect GOT slots ----
    // We need GOT slots before finalizing DATA vms, so first scan relocs.
    typedef struct { const char *name; uint64_t vm; } GotSlot;
    GotSlot *gots = NULL;
    size_t ngot = 0, gotcap = 0;
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls < 0) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        for (uint32_t r = 0; r < s->nreloc; r++) {
            const MachOReloc *mr = &s->relocs[r];
            if (mr->type != MACHO_RELOC_GOT_LOAD_PAGE21 &&
                mr->type != MACHO_RELOC_GOT_LOAD_PAGEOFF12 &&
                mr->type != MACHO_RELOC_POINTER_TO_GOT)
                continue;
            const char *nm = pl[i].obj->symbols[mr->sym].name;
            int found = 0;
            for (size_t g = 0; g < ngot; g++)
                if (strcmp(gots[g].name, nm) == 0) { found = 1; break; }
            if (!found) {
                if (ngot == gotcap) {
                    gotcap = gotcap ? gotcap * 2 : 4;
                    GotSlot *ng = realloc(gots, gotcap * sizeof(GotSlot));
                    if (!ng) abort();
                    gots = ng;
                }
                gots[ngot].name = nm;
                gots[ngot].vm = 0;
                ngot++;
            }
        }
    }
    uint64_t got_base = la_ptr_vm + next * 8;
    // got[0] reserved for dyld_stub_binder
    for (size_t g = 0; g < ngot; g++) gots[g].vm = got_base + (g + 1) * 8;
    uint64_t got_size = (1 + ngot) * 8;
    uint64_t data_end = got_base + got_size;
    uint64_t data_filesize = align_up(data_end - data_vm, PAGE);

    // Finish extern VM bookkeeping.
    for (size_t i = 0; i < next; i++) ext[i].stub_vm = stubs_vm + i * 12;

    // Resolve defined-symbol final addresses (needs placement lookup).
    // Build section -> placement map: for each object, placement index of
    // its section k.
    for (size_t i = 0; i < syms.n; i++) {
        ResolvedSym *s = &syms.v[i];
        if (s->kind != SYM_DEFINED) continue;
        // find defining placement: search all objects for a global symbol
        // with this name and defined section
        for (size_t o = 0; o < nobj; o++) {
            const MachOObject *obj = &objs[o];
            for (uint32_t k = 0; k < obj->nsymbol; k++) {
                const MachOSymbol *ms = &obj->symbols[k];
                if (!ms->global || ms->section < 0 || strcmp(ms->name, s->name) != 0)
                    continue;
                // locate placement
                size_t pidx = 0, run = 0;
                for (size_t o2 = 0; o2 < nobj && o2 <= o; o2++) {
                    if (o2 == o) { pidx = run + ms->section; break; }
                    run += objs[o2].nsection;
                }
                // Object symbol values are running addresses; subtract the
                // defining section's own address to get the section-relative
                // offset before adding the placed base.
                s->addr = pl[pidx].base_vm +
                          (ms->value - obj->sections[ms->section].addr);
                goto resolved;
            }
        }
        return fail(err, errlen, "link_macho: internal: cannot place '%s'", s->name);
    resolved:;
    }

    uint64_t entry_vm = ent->addr;

    // ---- pass 5: materialize __text (copy + apply relocations) ----
    uint8_t *textbuf = calloc(1, text_filesize);
    if (!textbuf) return fail(err, errlen, "link_macho: out of memory", NULL);
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls < 0 || pl[i].cls == 2 || pl[i].cls == 3) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        if (s->size == 0) continue;
        memcpy(textbuf + (pl[i].base_vm - TEXT_VM), s->data, s->size);
    }

    // Apply relocations for text-side and data-side sections.
    uint8_t *databuf = calloc(1, data_filesize ? data_filesize : 1);
    if (!databuf) return fail(err, errlen, "link_macho: out of memory", NULL);
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls != 2) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        if (s->size) memcpy(databuf + (pl[i].base_vm - data_vm), s->data, s->size);
    }

    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls < 0 || pl[i].cls == 3) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        int in_text = pl[i].cls <= 1;
        uint8_t *dst = in_text ? textbuf : databuf;
        uint64_t dst_base = in_text ? TEXT_VM : data_vm;
        for (uint32_t r = 0; r < s->nreloc; r++) {
            const MachOReloc *mr = &s->relocs[r];
            const MachOSymbol *rs = &pl[i].obj->symbols[mr->sym];
            uint64_t patch = pl[i].base_vm + mr->address;
            uint8_t *field = dst + (patch - dst_base);
            // Resolve target VM.
            uint64_t target = 0;
            int got_ref = 0;
            ResolvedSym *def = symtab_find(&syms, rs->name);
            if (rs->section >= 0 && !(def && def->kind == SYM_COMMON)) {
                // Defined-in-section symbol: running address -> section
                // relative by subtracting the section's own address.
                size_t pidx = 0, run = 0;
                for (size_t o2 = 0; o2 < nobj; o2++) {
                    if (&objs[o2] == pl[i].obj) { pidx = run + rs->section; break; }
                    run += objs[o2].nsection;
                }
                target = pl[pidx].base_vm +
                         (rs->value - pl[i].obj->sections[rs->section].addr);
            } else if (def && def->kind == SYM_DEFINED) {
                target = def->addr;
            } else if (def && def->kind == SYM_COMMON) {
                target = def->addr;
            } else if (def && def->kind == SYM_EXTERN) {
                if (mr->type == MACHO_RELOC_GOT_LOAD_PAGE21 ||
                    mr->type == MACHO_RELOC_GOT_LOAD_PAGEOFF12 ||
                    mr->type == MACHO_RELOC_POINTER_TO_GOT) {
                    // GOT indirection: target resolved to the GOT slot below.
                } else if (mr->type != MACHO_RELOC_BRANCH26) {
                    return fail(err, errlen,
                                "link_macho: unsupported extern reloc on '%s'", rs->name);
                } else {
                    if (def->ext < 0)
                        return fail(err, errlen, "link_macho: extern '%s' has no stub", rs->name);
                    target = ext[def->ext].stub_vm;
                }
            } else {
                return fail(err, errlen, "link_macho: unresolved reloc target '%s'", rs->name);
            }
            if (mr->type == MACHO_RELOC_GOT_LOAD_PAGE21 ||
                mr->type == MACHO_RELOC_GOT_LOAD_PAGEOFF12 ||
                mr->type == MACHO_RELOC_POINTER_TO_GOT) {
                got_ref = 1;
                int found = 0;
                for (size_t g = 0; g < ngot; g++)
                    if (strcmp(gots[g].name, rs->name) == 0) { target = gots[g].vm; found = 1; }
                if (!found)
                    return fail(err, errlen,
                                "link_macho: internal: no GOT slot for '%s'", rs->name);
            }
            switch (mr->type) {
            case MACHO_RELOC_BRANCH26: {
                uint32_t ins = rd_u32(field);
                int64_t imm = (int64_t)(target - patch) / 4;
                wr_u32(field, (ins & 0xFC000000u) | ((uint32_t)imm & 0x03FFFFFFu));
                break;
            }
            case MACHO_RELOC_PAGE21:
            case MACHO_RELOC_GOT_LOAD_PAGE21: {
                uint32_t ins = rd_u32(field);
                int64_t imm = (int64_t)(target >> 12) - (int64_t)(patch >> 12);
                uint32_t immlo = (uint32_t)(imm & 3);
                uint32_t immhi = (uint32_t)((imm >> 2) & ((1 << 19) - 1));
                wr_u32(field, (ins & 0x9F00001Fu) | (immlo << 29) | (immhi << 5));
                break;
            }
            case MACHO_RELOC_PAGEOFF12:
            case MACHO_RELOC_GOT_LOAD_PAGEOFF12: {
                uint32_t ins = rd_u32(field);
                uint32_t off12 = (uint32_t)(target & 0xFFFu);
                if ((ins & 0xFFC00000u) == 0xF9400000u) off12 /= 8; // ldr x scaled
                wr_u32(field, (ins & 0xFFC003FFu) | ((off12 & 0xFFFu) << 10));
                break;
            }
            case MACHO_RELOC_POINTER_TO_GOT:
            case MACHO_RELOC_UNSIGNED:
                wr_u64(field, target);
                (void)got_ref;
                break;
            case MACHO_RELOC_SUBTRACTOR:
                return fail(err, errlen,
                            "link_macho: SUBTRACTOR reloc outside compact unwind", NULL);
            default:
                return fail(err, errlen, "link_macho: unsupported reloc type", NULL);
            }
        }
    }

    // ---- pass 6: emit stubs + stub helper ----
    for (size_t i = 0; i < next; i++) {
        uint64_t sv = ext[i].stub_vm;
        wr_u32(textbuf + (sv - TEXT_VM) + 0, enc_adrp(sv, ext[i].la_ptr_vm, 16));
        wr_u32(textbuf + (sv - TEXT_VM) + 4,
               enc_ldr_imm64(16, 16, (uint32_t)(ext[i].la_ptr_vm & 0xFFFu)));
        wr_u32(textbuf + (sv - TEXT_VM) + 8, 0xD61F0200u); // br x16
    }
    if (next > 0) {
        wr_u32(textbuf + (helper_vm - TEXT_VM) + 0, enc_adrp(helper_vm, got_base, 16));
        wr_u32(textbuf + (helper_vm - TEXT_VM) + 4,
               enc_ldr_imm64(16, 16, (uint32_t)(got_base & 0xFFFu)));
        wr_u32(textbuf + (helper_vm - TEXT_VM) + 8, 0xD61F0200u); // br x16
    }
    // Helper entries (8 bytes each): movz w17, #lazyoff ; b header.
    for (size_t i = 0; i < next; i++) {
        uint64_t pv = helper_vm + 12 + i * 8;
        wr_u32(textbuf + (pv - TEXT_VM) + 0, enc_movz_w(17, ext[i].lazy_off & 0xFFFF));
        wr_u32(textbuf + (pv - TEXT_VM) + 4, enc_b(pv + 4, helper_vm));
    }

    // la_ptr initial values point at the per-symbol helper entry; the
    // executable is non-PIE so absolute values need no rebase info.
    for (size_t i = 0; i < next; i++)
        wr_u64(databuf + (ext[i].la_ptr_vm - data_vm), helper_vm + 12 + i * 8);

    // ---- pass 7: LINKEDIT payloads ----
    // Symbol table order: locals (none exported here), defined globals,
    // undefined (dyld_stub_binder + externs).
    Buf strtab = {0};
    buf_u8(&strtab, 0);
    Buf symtab = {0};
    StrTab st = { &strtab, NULL, 0, 0 };

    uint32_t n_local = 0, n_defined = 0;
    for (size_t i = 0; i < syms.n; i++) {
        ResolvedSym *s = &syms.v[i];
        if (s->kind == SYM_DEFINED) {
            uint32_t strx = strtb_intern(&st, s->name);
            buf_u32(&symtab, strx);
            buf_u8(&symtab, 0x0Fu); // N_SECT | N_EXT
            buf_u8(&symtab, 1);     // section 1 = __text (n_sect; fine for data too in exe)
            buf_u16(&symtab, 0);
            buf_u64(&symtab, s->addr);
            n_defined++;
        }
    }
    uint32_t binder_symidx = 0;
    {
        uint32_t strx = strtb_intern(&st, "_dyld_stub_binder");
        binder_symidx = n_local + n_defined;
        buf_u32(&symtab, strx);
        buf_u8(&symtab, 0x01u); // N_UNDF | N_EXT
        buf_u8(&symtab, 0);
        buf_u16(&symtab, 0);
        buf_u64(&symtab, 0);
    }
    uint32_t *ext_symidx = calloc(next ? next : 1, sizeof(uint32_t));
    for (size_t i = 0; i < next; i++) {
        uint32_t strx = strtb_intern(&st, ext[i].name);
        ext_symidx[i] = n_local + n_defined + 1 + (uint32_t)i;
        buf_u32(&symtab, strx);
        buf_u8(&symtab, 0x01u);
        buf_u8(&symtab, 0);
        buf_u16(&symtab, 0);
        buf_u64(&symtab, 0);
    }
    uint32_t nsyms_out = n_local + n_defined + 1 + (uint32_t)next;

    // Indirect symbol table: __stubs entries, then __la_symbol_ptr,
    // then __got (slot 0 = dyld_stub_binder; the rest are bound
    // non-lazy, marked INDIRECT_SYMBOL_ABS). Every slot of each
    // symbol-pointer/stub section must have an entry or codesign's
    // strict validation rejects the image.
    Buf indsym = {0};
    for (size_t i = 0; i < next; i++) buf_u32(&indsym, ext_symidx[i]);
    for (size_t i = 0; i < next; i++) buf_u32(&indsym, ext_symidx[i]);
    buf_u32(&indsym, binder_symidx);
    for (size_t gidx = 0; gidx < ngot; gidx++) buf_u32(&indsym, 0x40000000u);

    // Bind streams. Segment indices: PAGEZERO=0, TEXT=1, DATA=2.
    Buf bind = {0};
    // non-lazy: got[0] -> dyld_stub_binder, then got slots for GOT refs
    buf_u8(&bind, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1);
    buf_u8(&bind, BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
    // dyld exports the binder without the C underscore prefix; the symtab
    // entry keeps "_dyld_stub_binder" but the bind stream must match dyld.
    buf_put(&bind, "dyld_stub_binder\0", 17);
    buf_u8(&bind, BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);
    buf_u8(&bind, BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 2);
    buf_uleb(&bind, got_base - data_vm);
    buf_u8(&bind, BIND_OPCODE_DO_BIND);
    for (size_t g = 0; g < ngot; g++) {
        buf_u8(&bind, BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
        buf_put(&bind, gots[g].name, strlen(gots[g].name) + 1);
        buf_u8(&bind, BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 2);
        buf_uleb(&bind, gots[g].vm - data_vm);
        buf_u8(&bind, BIND_OPCODE_DO_BIND);
    }
    buf_u8(&bind, BIND_OPCODE_DONE);
    uint8_t export_trie = 0; // empty trie: single zero byte

    // ---- pass 8: layout LINKEDIT, build load commands ----
    uint64_t linkedit_off = text_filesize + data_filesize;
    Buf le = {0};
    size_t symtab_le = le.len; buf_pad(&le, 8);
    symtab_le = le.len; buf_put(&le, symtab.bytes, symtab.len);
    size_t strtab_le = le.len; buf_pad(&le, 8);
    strtab_le = le.len; buf_put(&le, strtab.bytes, strtab.len);
    size_t indsym_le = le.len; buf_pad(&le, 8);
    indsym_le = le.len; buf_put(&le, indsym.bytes, indsym.len);
    size_t bind_le = le.len; buf_pad(&le, 8);
    bind_le = le.len; buf_put(&le, bind.bytes, bind.len);
    size_t lazy_le = le.len; buf_pad(&le, 8);
    lazy_le = le.len; buf_put(&le, lazy.bytes, lazy.len);
    size_t export_le = le.len; buf_pad(&le, 8);
    export_le = le.len; buf_put(&le, &export_trie, 1);

    uint64_t linkedit_content = linkedit_off + le.len;
    uint64_t sig_dataoff = align_up(linkedit_content, PAGE); // codeLimit

    // ---- signature (validated ad-hoc recipe) ----
    char ident[64];
    snprintf(ident, sizeof(ident), "tiq");
    size_t ident_len = strlen(ident) + 1;
    uint64_t npages = sig_dataoff / PAGE;
    size_t hash_off = (size_t)(48 + ident_len + 2 * 32);
    size_t cd_len = hash_off + (size_t)npages * 32;
    size_t sig_len = 12 + 3 * 8 + cd_len + 12 + 8; // superblob + cd + reqs + wrap


    uint32_t n_text_sects = 3 + (cstr_size ? 1 : 0);
    uint32_t n_data_sects = 0;
    for (size_t i = 0; i < npl; i++) if (pl[i].cls == 2) n_data_sects++;
    if (common_size) n_data_sects++;
    if (next) n_data_sects += 1; // la_symbol_ptr
    if (next || ngot) n_data_sects += 1; // got

    // Assemble load-command buffer.
    Buf cmds = {0};
    // __PAGEZERO
    buf_u32(&cmds, LC_SEGMENT_64); buf_u32(&cmds, 72);
    buf_name(&cmds, "__PAGEZERO");
    buf_u64(&cmds, 0); buf_u64(&cmds, TEXT_VM);
    buf_u64(&cmds, 0); buf_u64(&cmds, 0);
    buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    // __TEXT
    buf_u32(&cmds, LC_SEGMENT_64); buf_u32(&cmds, 72 + 80 * n_text_sects);
    buf_name(&cmds, "__TEXT");
    buf_u64(&cmds, TEXT_VM); buf_u64(&cmds, text_filesize);
    buf_u64(&cmds, 0); buf_u64(&cmds, text_filesize);
    buf_u32(&cmds, 7); buf_u32(&cmds, 5); buf_u32(&cmds, n_text_sects); buf_u32(&cmds, 0);
    {
        uint64_t text_size = stubs_vm - text_vm;
        buf_name(&cmds, "__text"); buf_name(&cmds, "__TEXT");
        buf_u64(&cmds, text_vm); buf_u64(&cmds, text_size);
        buf_u32(&cmds, (uint32_t)(text_vm - TEXT_VM)); buf_u32(&cmds, 2);
        buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        buf_u32(&cmds, 0x80000400u); buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        if (next) {
            buf_name(&cmds, "__stubs"); buf_name(&cmds, "__TEXT");
            buf_u64(&cmds, stubs_vm); buf_u64(&cmds, stubs_size);
            buf_u32(&cmds, (uint32_t)(stubs_vm - TEXT_VM)); buf_u32(&cmds, 2);
            buf_u32(&cmds, 0); buf_u32(&cmds, 0);
            buf_u32(&cmds, 0x80000008u); buf_u32(&cmds, 0); buf_u32(&cmds, 12); buf_u32(&cmds, 0);
            buf_name(&cmds, "__stub_helper"); buf_name(&cmds, "__TEXT");
            buf_u64(&cmds, helper_vm); buf_u64(&cmds, helper_size);
            buf_u32(&cmds, (uint32_t)(helper_vm - TEXT_VM)); buf_u32(&cmds, 2);
            buf_u32(&cmds, 0); buf_u32(&cmds, 0);
            buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        }
        if (cstr_size) {
            buf_name(&cmds, "__cstring"); buf_name(&cmds, "__TEXT");
            buf_u64(&cmds, cstr_vm); buf_u64(&cmds, cstr_size);
            buf_u32(&cmds, (uint32_t)(cstr_vm - TEXT_VM)); buf_u32(&cmds, 0);
            buf_u32(&cmds, 0); buf_u32(&cmds, 0);
            buf_u32(&cmds, 0x2u); buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        }
    }
    // __DATA
    buf_u32(&cmds, LC_SEGMENT_64); buf_u32(&cmds, 72 + 80 * n_data_sects);
    buf_name(&cmds, "__DATA");
    buf_u64(&cmds, data_vm); buf_u64(&cmds, data_filesize);
    buf_u64(&cmds, text_filesize); buf_u64(&cmds, data_filesize);
    buf_u32(&cmds, 7); buf_u32(&cmds, 3); buf_u32(&cmds, n_data_sects); buf_u32(&cmds, 0);
    for (size_t i = 0; i < npl; i++) {
        if (pl[i].cls != 2) continue;
        const MachOSection *s = &pl[i].obj->sections[pl[i].sect];
        buf_name(&cmds, "__data"); buf_name(&cmds, "__DATA");
        buf_u64(&cmds, pl[i].base_vm); buf_u64(&cmds, s->size);
        buf_u32(&cmds, (uint32_t)(text_filesize + (pl[i].base_vm - data_vm)));
        buf_u32(&cmds, 3);
        buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    }
    if (common_size) {
        buf_name(&cmds, "__common"); buf_name(&cmds, "__DATA");
        buf_u64(&cmds, common_vm); buf_u64(&cmds, common_size);
        buf_u32(&cmds, 0); buf_u32(&cmds, 3);
        buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        buf_u32(&cmds, 0x1u /* S_ZEROFILL */); buf_u32(&cmds, 0); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    }
    if (next) {
        buf_name(&cmds, "__la_symbol_ptr"); buf_name(&cmds, "__DATA");
        buf_u64(&cmds, la_ptr_vm); buf_u64(&cmds, next * 8);
        buf_u32(&cmds, (uint32_t)(text_filesize + (la_ptr_vm - data_vm))); buf_u32(&cmds, 3);
        buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        buf_u32(&cmds, 0x7u); buf_u32(&cmds, (uint32_t)next); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    }
    if (next || ngot) {
        buf_name(&cmds, "__got"); buf_name(&cmds, "__DATA");
        buf_u64(&cmds, got_base); buf_u64(&cmds, got_size);
        buf_u32(&cmds, (uint32_t)(text_filesize + (got_base - data_vm))); buf_u32(&cmds, 3);
        buf_u32(&cmds, 0); buf_u32(&cmds, 0);
        buf_u32(&cmds, 0x6u); buf_u32(&cmds, (uint32_t)(next * 2)); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    }
    // __LINKEDIT
    uint64_t linkedit_filesize = sig_dataoff - linkedit_off + sig_len;
    buf_u32(&cmds, LC_SEGMENT_64); buf_u32(&cmds, 72);
    buf_name(&cmds, "__LINKEDIT");
    buf_u64(&cmds, TEXT_VM + text_filesize + data_filesize);
    buf_u64(&cmds, align_up(linkedit_filesize, PAGE));
    buf_u64(&cmds, linkedit_off); buf_u64(&cmds, linkedit_filesize);
    buf_u32(&cmds, 1); buf_u32(&cmds, 1); buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    // LC_MAIN
    buf_u32(&cmds, LC_MAIN); buf_u32(&cmds, 24);
    buf_u64(&cmds, entry_vm - TEXT_VM); buf_u64(&cmds, 0);
    // LC_DYLD_INFO_ONLY
    buf_u32(&cmds, LC_DYLD_INFO_ONLY); buf_u32(&cmds, 48);
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    buf_u32(&cmds, (uint32_t)(linkedit_off + bind_le)); buf_u32(&cmds, (uint32_t)bind.len);
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);
    buf_u32(&cmds, (uint32_t)(linkedit_off + lazy_le)); buf_u32(&cmds, (uint32_t)lazy.len);
    buf_u32(&cmds, (uint32_t)(linkedit_off + export_le)); buf_u32(&cmds, 1);
    // LC_SYMTAB
    buf_u32(&cmds, LC_SYMTAB); buf_u32(&cmds, 24);
    buf_u32(&cmds, (uint32_t)(linkedit_off + symtab_le)); buf_u32(&cmds, nsyms_out);
    buf_u32(&cmds, (uint32_t)(linkedit_off + strtab_le)); buf_u32(&cmds, (uint32_t)strtab.len);
    // LC_DYSYMTAB
    buf_u32(&cmds, LC_DYSYMTAB); buf_u32(&cmds, 80);
    buf_u32(&cmds, n_local);                // ilocalsym
    buf_u32(&cmds, n_local);                // nlocalsym
    buf_u32(&cmds, n_local);                // iextdefsym
    buf_u32(&cmds, n_defined);              // nextdefsym
    buf_u32(&cmds, n_local + n_defined);    // iundefsym
    buf_u32(&cmds, 1 + (uint32_t)next);     // nundefsym
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);   // tocoff, ntoc
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);   // modtaboff, nmodtab
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);   // extrefsymoff, nextrefsyms
    buf_u32(&cmds, (uint32_t)(linkedit_off + indsym_le));
    buf_u32(&cmds, (uint32_t)(indsym.len / 4));
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);   // extreloff, nextrel
    buf_u32(&cmds, 0); buf_u32(&cmds, 0);   // locreloff, nlocrel
    // LC_LOAD_DYLIB
    {
        const char *path = "/usr/lib/libSystem.B.dylib";
        size_t csz = align_up(24 + strlen(path) + 1, 8);
        buf_u32(&cmds, LC_LOAD_DYLIB); buf_u32(&cmds, (uint32_t)csz);
        buf_u32(&cmds, 24); buf_u32(&cmds, 2);
        buf_u32(&cmds, 0x10000); buf_u32(&cmds, 0x10000);
        buf_put(&cmds, path, strlen(path) + 1);
        while (cmds.len % 8) buf_u8(&cmds, 0);
    }
    // LC_LOAD_DYLINKER
    {
        const char *path = "/usr/lib/dyld";
        size_t csz = align_up(12 + strlen(path) + 1, 8);
        buf_u32(&cmds, LC_LOAD_DYLINKER); buf_u32(&cmds, (uint32_t)csz);
        buf_u32(&cmds, 12);
        buf_put(&cmds, path, strlen(path) + 1);
        while (cmds.len % 8) buf_u8(&cmds, 0);
    }
    // LC_UUID: derived from input content (deterministic)
    {
        Sha256 us;
        sha256_init(&us);
        for (size_t o = 0; o < nobj; o++)
            for (uint32_t i = 0; i < objs[o].nsection; i++) {
                const MachOSection *s = &objs[o].sections[i];
                if (s->data) sha256_update(&us, s->data, s->size);
            }
        uint8_t d[32];
        sha256_final(&us, d);
        buf_u32(&cmds, LC_UUID); buf_u32(&cmds, 24);
        buf_put(&cmds, d, 16);
    }
    // LC_BUILD_VERSION (macOS, tool ld)
    buf_u32(&cmds, LC_BUILD_VERSION); buf_u32(&cmds, 32);
    buf_u32(&cmds, 1);           // platform macOS
    buf_u32(&cmds, 14u << 16);   // minos 14.0
    buf_u32(&cmds, (14u << 16) | 5u); // sdk
    buf_u32(&cmds, 1);           // ntools
    buf_u32(&cmds, 3); buf_u32(&cmds, 1267u << 16); // ld
    // LC_CODE_SIGNATURE
    buf_u32(&cmds, LC_CODE_SIGNATURE); buf_u32(&cmds, 16);
    buf_u32(&cmds, (uint32_t)sig_dataoff); buf_u32(&cmds, (uint32_t)sig_len);

    if (32 + cmds.len > PAGE)
        return fail(err, errlen, "link_macho: load commands overflow page 0", NULL);
    uint32_t ncmds = 0;
    for (size_t p = 0; p + 8 <= cmds.len; ) {
        uint32_t sz = rd_u32(cmds.bytes + p + 4);
        p += sz;
        ncmds++;
    }

    // ---- final image assembly (trial loop for signature convergence) ----
    size_t total = (size_t)(sig_dataoff + sig_len);
    uint8_t *img = calloc(1, total);
    if (!img) return fail(err, errlen, "link_macho: out of memory", NULL);
    memcpy(img, textbuf, text_filesize);
    memcpy(img + text_filesize, databuf, data_filesize);
    memcpy(img + linkedit_off, le.bytes, le.len);

    for (int iter = 0; iter < 4; iter++) {
        // header
        wr_u32(img + 0, MH_MAGIC_64);
        wr_u32(img + 4, CPU_ARM64);
        wr_u32(img + 8, 0);
        wr_u32(img + 12, MH_EXECUTE);
        wr_u32(img + 16, ncmds);
        wr_u32(img + 20, (uint32_t)cmds.len);
        wr_u32(img + 24, MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE);
        wr_u32(img + 28, 0);
        memcpy(img + 32, cmds.bytes, cmds.len);

        // signature over pages [0, sig_dataoff)
        static const uint8_t req_blob[12] = {
            0xFA,0xDE,0x0C,0x01, 0,0,0,12, 0,0,0,0
        };
        uint8_t req_hash[32];
        sha256(req_blob, sizeof(req_blob), req_hash);
        Buf cd = {0};
        buf_u32(&cd, 0xFADE0C02u);                 // magic (BE on disk, same bytes)
        buf_u32(&cd, (uint32_t)cd_len);
        buf_u32(&cd, 0x00020100u);                 // version
        buf_u32(&cd, 0x00000002u);                 // flags: adhoc
        buf_u32(&cd, (uint32_t)hash_off);
        buf_u32(&cd, 48);                          // identOffset
        buf_u32(&cd, 2);                           // nSpecialSlots
        buf_u32(&cd, (uint32_t)npages);
        buf_u32(&cd, (uint32_t)sig_dataoff);       // codeLimit
        buf_u8(&cd, 32); buf_u8(&cd, 2); buf_u8(&cd, 0); buf_u8(&cd, 14);
        buf_u32(&cd, 0);                           // spare2
        while (cd.len < 48) buf_u8(&cd, 0);
        buf_put(&cd, ident, ident_len);
        buf_put(&cd, req_hash, 32);                // slot -2: requirements
        uint8_t zeros32[32] = {0};
        buf_put(&cd, zeros32, 32);                 // slot -1: absent
        for (uint64_t pg = 0; pg < npages; pg++) {
            uint8_t ph[32];
            sha256(img + pg * PAGE, PAGE, ph);
            buf_put(&cd, ph, 32);
        }
        // NOTE: CD fields above are little-endian on disk; the kernel and
        // codesign read them as big-endian per CS spec. Swap the nine u32
        // fields in place (stop before offset 36: the hashSize/hashType/
        // platform/pageSize bytes are already byte-oriented).
        for (size_t w = 0; w + 4 <= 36; w += 4) {
            uint8_t t0 = cd.bytes[w], t1 = cd.bytes[w+1];
            cd.bytes[w] = cd.bytes[w+3]; cd.bytes[w+1] = cd.bytes[w+2];
            cd.bytes[w+2] = t1; cd.bytes[w+3] = t0;
        }
        Buf sb = {0};
        uint32_t cd_off = 12 + 3 * 8;
        uint32_t req_off = cd_off + (uint32_t)cd.len;
        uint32_t wrap_off = req_off + (uint32_t)sizeof(req_blob);
        buf_u32(&sb, 0xFADE0CC0u);
        buf_u32(&sb, wrap_off + 8);
        buf_u32(&sb, 3);
        buf_u32(&sb, 0x0000u); buf_u32(&sb, cd_off);
        buf_u32(&sb, 0x0002u); buf_u32(&sb, req_off);
        buf_u32(&sb, 0x00010000u); buf_u32(&sb, wrap_off);
        buf_put(&sb, cd.bytes, cd.len);
        buf_put(&sb, req_blob, sizeof(req_blob));
        static const uint8_t wrap_blob[8] = { 0xFA,0xDE,0x0B,0x01, 0,0,0,8 };
        buf_put(&sb, wrap_blob, sizeof(wrap_blob));
        // big-endian swap for superblob header + index
        for (size_t w = 0; w < 12 + 3 * 8; w += 4) {
            uint8_t t0 = sb.bytes[w], t1 = sb.bytes[w+1];
            sb.bytes[w] = sb.bytes[w+3]; sb.bytes[w+1] = sb.bytes[w+2];
            sb.bytes[w+2] = t1; sb.bytes[w+3] = t0;
        }
        if (sb.len != sig_len) { free(sb.bytes); free(cd.bytes);
            return fail(err, errlen, "link_macho: internal: signature size mismatch", NULL); }
        memcpy(img + sig_dataoff, sb.bytes, sb.len);
        free(sb.bytes);
        free(cd.bytes);
        break; // sizes are precomputed; one pass suffices
    }

    free(textbuf);
    free(databuf);
    free(pl);
    free(syms.v);
    free(ext);
    free(gots);
    free(ext_symidx);
    free(st.v);
    free(strtab.bytes); free(symtab.bytes); free(indsym.bytes);
    free(bind.bytes); free(lazy.bytes); free(le.bytes); free(cmds.bytes);

    *out = img;
    *out_len = total;
    return 0;
}
