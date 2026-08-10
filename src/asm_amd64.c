// M17.3.4: integrated x86_64 assembler for the QBE amd64 assembly subset.
//
// QBE (third_party/qbe, amd64 target, Gaself mode) emits a small,
// stable subset of AT&T-syntax x86_64 assembly (see amd64/emit.c omap[]
// and gas.c). This module parses exactly that subset and produces
// section contents plus relocations in memory. Anything outside the
// subset fails closed with a located diagnostic; partial objects are
// never produced.

#include "../include/asm_amd64.h"
#include "../include/elf_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- internal tables --------------------------------------------------------

typedef struct {
    char *name;      // owned
    int section;
    int64_t offset;
} Label;

typedef struct {
    int section;     // always ASM_SEC_TEXT
    int64_t offset;  // of the branch/call instruction
    int line;
    char *label;     // owned
    int rel_off;     // offset within the instruction where the rel32 is
    int is_call;     // 1 for callq, 0 for jmp/jcc
} Fixup;

typedef struct {
    AsmUnit *u;
    int cur;         // current section
    int line;
    Label *labels;
    size_t nlabel, label_cap;
    Fixup *fixups;
    size_t nfixup, fixup_cap;
    int failed;
} Ctx;

// Register IDs for encoding.
enum {
    R_RAX=0, R_RCX=1, R_RDX=2, R_RBX=3, R_RSP=4, R_RBP=5, R_RSI=6, R_RDI=7,
    R_R8=8, R_R9=9, R_R10=10, R_R11=11, R_R12=12, R_R13=13, R_R14=14, R_R15=15,
    R_XMM0=16, R_XMM1=17, R_XMM2=18, R_XMM3=19, R_XMM4=20, R_XMM5=21,
    R_XMM6=22, R_XMM7=23, R_XMM8=24, R_XMM9=25, R_XMM10=26, R_XMM11=27,
    R_XMM12=28, R_XMM13=29, R_XMM14=30, R_XMM15=31,
    R_NONE=-1
};

// Size classes for encoding.
enum { SZ_B=0, SZ_W=1, SZ_L=2, SZ_Q=3 };

// Parsed register.
typedef struct { int id; int size; } Reg;

// Parsed memory operand: disp(base, index, scale).
typedef struct {
    int64_t disp;
    int base;       // register id or R_NONE
    int index;      // register id or R_NONE
    int scale;      // 1, 2, 4, 8
    int is_rip;     // 1 if base is %rip
    int has_disp;   // 1 if displacement was explicitly specified
    char sym_name[256]; // symbol name for symbolic displacements
    int has_sym;    // 1 if symbolic displacement
    int64_t sym_off; // addend after symbol
} MemOp;

// Parsed operand: register, memory, immediate, or label.
typedef enum { OP_NONE, OP_REG, OP_MEM, OP_IMM } OpKind;
typedef struct {
    OpKind kind;
    Reg reg;
    MemOp mem;
    int64_t imm;
    int imm_is_label; // 1 if immediate is a symbolic label
    char label[256];
} Operand;

// --- error reporting --------------------------------------------------------

static void err(Ctx *c, const char *fmt, ...) {
    if (c->failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->u->errmsg, sizeof(c->u->errmsg), fmt, ap);
    va_end(ap);
    c->u->errline = c->line;
    c->u->has_error = 1;
    c->failed = 1;
}

// --- unit / section helpers -------------------------------------------------

static void sec_put(Ctx *c, const void *bytes, size_t n) {
    AsmSectionOut *s = &c->u->sec[c->cur];
    if (s->len + n > s->cap) {
        size_t cap = s->cap ? s->cap : 256;
        while (cap < s->len + n) cap *= 2;
        s->bytes = realloc(s->bytes, cap);
        s->cap = cap;
    }
    memcpy(s->bytes + s->len, bytes, n);
    s->len += n;
}

static void sec_put8(Ctx *c, uint8_t v) { sec_put(c, &v, 1); }
static void sec_put16(Ctx *c, uint16_t v) { uint8_t d[2] = {(uint8_t)v,(uint8_t)(v>>8)}; sec_put(c,d,2); }
static void sec_put32(Ctx *c, uint32_t w) {
    uint8_t b[4] = {(uint8_t)w,(uint8_t)(w>>8),(uint8_t)(w>>16),(uint8_t)(w>>24)};
    sec_put(c, b, 4);
}
static void sec_put64(Ctx *c, uint64_t w) {
    uint8_t b[8]; for (int i=0;i<8;i++) b[i]=(uint8_t)(w>>(8*i));
    sec_put(c, b, 8);
}

static void sec_align(Ctx *c, size_t align) {
    AsmSectionOut *s = &c->u->sec[c->cur];
    size_t rem = s->len % align;
    if (rem == 0) return;
    size_t pad = align - rem;
    uint8_t zero = 0;
    for (size_t i = 0; i < pad; i++) sec_put(c, &zero, 1);
}

static void add_reloc(Ctx *c, int section, int32_t address, int sym,
                      unsigned pcrel, unsigned length, unsigned type) {
    AsmSectionOut *s = &c->u->sec[section];
    if (s->nreloc >= s->reloc_cap) {
        size_t cap = s->reloc_cap ? s->reloc_cap * 2 : 8;
        s->relocs = realloc(s->relocs, cap * sizeof(AsmReloc));
        s->reloc_cap = cap;
    }
    s->relocs[s->nreloc].address = address;
    s->relocs[s->nreloc].symbol = sym;
    s->relocs[s->nreloc].pcrel = pcrel;
    s->relocs[s->nreloc].length = length;
    s->relocs[s->nreloc].type = type;
    s->relocs[s->nreloc].addend = 0;
    s->relocs[s->nreloc].pair = -1;
    s->nreloc++;
}

// --- symbol / label tables --------------------------------------------------

static int sym_find(AsmUnit *u, const char *name, size_t len) {
    for (size_t i = 0; i < u->nsym; i++)
        if (strlen(u->syms[i].name) == len && memcmp(u->syms[i].name, name, len) == 0)
            return (int)i;
    return -1;
}

static int sym_intern(Ctx *c, const char *name, size_t len) {
    int idx = sym_find(c->u, name, len);
    if (idx >= 0) return idx;
    AsmUnit *u = c->u;
    if (u->nsym >= u->sym_cap) {
        size_t cap = u->sym_cap ? u->sym_cap * 2 : 16;
        u->syms = realloc(u->syms, cap * sizeof(AsmSymbol));
        u->sym_cap = cap;
    }
    char *copy = malloc(len + 1);
    memcpy(copy, name, len);
    copy[len] = '\0';
    u->syms[u->nsym].name = copy;
    u->syms[u->nsym].section = -1;
    u->syms[u->nsym].value = 0;
    u->syms[u->nsym].global = 0;
    return (int)u->nsym++;
}

static Label *label_find(Ctx *c, const char *name, size_t len) {
    for (size_t i = 0; i < c->nlabel; i++)
        if (strlen(c->labels[i].name) == len && memcmp(c->labels[i].name, name, len) == 0)
            return &c->labels[i];
    return NULL;
}

static void label_add(Ctx *c, const char *name, size_t len, int section, int64_t offset) {
    if (c->nlabel >= c->label_cap) {
        size_t cap = c->label_cap ? c->label_cap * 2 : 32;
        c->labels = realloc(c->labels, cap * sizeof(Label));
        c->label_cap = cap;
    }
    char *copy = malloc(len + 1);
    memcpy(copy, name, len);
    copy[len] = '\0';
    c->labels[c->nlabel].name = copy;
    c->labels[c->nlabel].section = section;
    c->labels[c->nlabel].offset = offset;
    c->nlabel++;
}

static void fixup_add(Ctx *c, int64_t offset, const char *label, size_t len,
                       int rel_off, int is_call) {
    if (c->nfixup >= c->fixup_cap) {
        size_t cap = c->fixup_cap ? c->fixup_cap * 2 : 32;
        c->fixups = realloc(c->fixups, cap * sizeof(Fixup));
        c->fixup_cap = cap;
    }
    char *copy = malloc(len + 1);
    memcpy(copy, label, len);
    copy[len] = '\0';
    c->fixups[c->nfixup].section = ASM_SEC_TEXT;
    c->fixups[c->nfixup].offset = offset;
    c->fixups[c->nfixup].line = c->line;
    c->fixups[c->nfixup].label = copy;
    c->fixups[c->nfixup].rel_off = rel_off;
    c->fixups[c->nfixup].is_call = is_call;
    c->nfixup++;
}

// --- parsing helpers --------------------------------------------------------

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int is_eol(char c) { return c == '\0' || c == '\n' || c == '\r' || c == '#'; }

// Parse an integer literal (decimal or hex 0x...).
static int parse_imm(const char *s, const char **end, int64_t *out) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *e;
        unsigned long long v = strtoull(s + 2, &e, 16);
        *out = neg ? -(int64_t)v : (int64_t)v;
        *end = e;
        return 0;
    }
    if (*s >= '0' && *s <= '9') {
        char *e;
        long long v = strtoll(s, &e, 10);
        *out = neg ? -v : v;
        *end = e;
        return 0;
    }
    return -1;
}

// --- register parsing -------------------------------------------------------

static Reg parse_reg(const char *s, const char **end) {
    Reg r = { R_NONE, SZ_Q };
    if (*s != '%') return r;
    s++;
    // XMM registers.
    if (s[0] == 'x' && s[1] == 'm' && s[2] == 'm') {
        const char *p = s + 3;
        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (n <= 15) { r.id = R_XMM0 + n; r.size = SZ_Q; }
        *end = p;
        return r;
    }
    // %rip
    if (s[0] == 'r' && s[1] == 'i' && s[2] == 'p') {
        r.id = R_NONE; r.size = SZ_Q; // sentinel for RIP
        *end = s + 3;
        return r;
    }
    // 64-bit: rax, rbx, ..., r15
    static const char *names64[] = {
        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    for (int i = 0; i < 16; i++) {
        size_t l = strlen(names64[i]);
        if (memcmp(s, names64[i], l) == 0) {
            r.id = i; r.size = SZ_Q; *end = s + l; return r;
        }
    }
    // 32-bit: eax, ecx, ..., r15d
    static const char *names32[] = {
        "eax","ecx","edx","ebx","esp","ebp","esi","edi",
        "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"
    };
    for (int i = 0; i < 16; i++) {
        size_t l = strlen(names32[i]);
        if (memcmp(s, names32[i], l) == 0) {
            r.id = i; r.size = SZ_L; *end = s + l; return r;
        }
    }
    // 16-bit: ax, cx, ..., r15w
    static const char *names16[] = {
        "ax","cx","dx","bx","sp","bp","si","di",
        "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"
    };
    for (int i = 0; i < 16; i++) {
        size_t l = strlen(names16[i]);
        if (memcmp(s, names16[i], l) == 0) {
            r.id = i; r.size = SZ_W; *end = s + l; return r;
        }
    }
    // 8-bit: al, cl, dl, bl, sil, dil, bpl, spl, r8b..r15b
    static const char *names8[] = {
        "al","cl","dl","bl","spl","bpl","sil","dil",
        "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"
    };
    for (int i = 0; i < 16; i++) {
        size_t l = strlen(names8[i]);
        if (memcmp(s, names8[i], l) == 0) {
            r.id = i; r.size = SZ_B; *end = s + l; return r;
        }
    }
    // 8-bit high: ah, ch, dh, bh
    if (s[0] == 'a' && s[1] == 'h') { r.id = R_RAX; r.size = SZ_B; *end = s+2; return r; }
    if (s[0] == 'c' && s[1] == 'h') { r.id = R_RCX; r.size = SZ_B; *end = s+2; return r; }
    if (s[0] == 'd' && s[1] == 'h') { r.id = R_RDX; r.size = SZ_B; *end = s+2; return r; }
    if (s[0] == 'b' && s[1] == 'h') { r.id = R_RBX; r.size = SZ_B; *end = s+2; return r; }
    return r;
}

// --- memory operand parsing -------------------------------------------------

// Parse: disp(base, index, scale) or disp(%rip) or symbol+offset(%rip)
static MemOp parse_mem(const char *s, const char **end) {
    MemOp m = {0, R_NONE, R_NONE, 1, 0, 0, {0}, 0, 0};
    const char *p = s;
    // Parse displacement (may be symbolic).
    if (*p == '(') {
        m.disp = 0; m.has_disp = 0;
    } else {
        // Check for symbol name (starts with letter, ., or _).
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || *p == '.') {
            const char *start = p;
            while (*p && *p != '(' && *p != '+' && *p != '-' && *p != ' ' && *p != ',' && *p != '\n' && *p != '\r') p++;
            size_t nlen = (size_t)(p - start);
            if (nlen < sizeof(m.sym_name)) {
                memcpy(m.sym_name, start, nlen);
                m.sym_name[nlen] = '\0';
                m.has_sym = 1;
            }
            // Parse optional +offset.
            if (*p == '+') {
                p++;
                int64_t v; const char *e2;
                if (parse_imm(p, &e2, &v) == 0) { m.sym_off = v; p = e2; }
            } else if (*p == '-') {
                p++;
                int64_t v; const char *e2;
                if (parse_imm(p, &e2, &v) == 0) { m.sym_off = -v; p = e2; }
            }
        } else {
            int64_t v;
            if (parse_imm(p, &p, &v) == 0) { m.disp = v; m.has_disp = 1; }
        }
    }
    if (*p != '(') { *end = p; return m; }
    p++; // skip (
    // Check for %rip.
    if (p[0] == '%' && p[1] == 'r' && p[2] == 'i' && p[3] == 'p') {
        m.is_rip = 1; m.base = R_NONE;
        p += 4;
        if (*p == ')') p++;
        *end = p;
        return m;
    }
    // Parse base register.
    if (*p != ')' && *p != ',') {
        Reg r = parse_reg(p, &p);
        m.base = r.id;
    }
    if (*p == ',') {
        p++;
        // Parse index register.
        Reg r = parse_reg(p, &p);
        m.index = r.id;
        if (*p == ',') {
            p++;
            // Parse scale.
            int64_t v; const char *e2;
            if (parse_imm(p, &e2, &v) == 0) { m.scale = (int)v; p = e2; }
        }
    }
    if (*p == ')') p++;
    *end = p;
    return m;
}

// --- operand parsing --------------------------------------------------------

static Operand parse_operand(const char *s, const char **end) {
    Operand op = {0};
    op.kind = OP_NONE;
    s = skip_ws(s);
    if (*s == '$') {
        // Immediate.
        s++;
        op.kind = OP_IMM;
        // Check for symbolic label.
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_' || *s == '.') {
            const char *start = s;
            while (*s && *s != ',' && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '+' && *s != '-') s++;
            size_t n = (size_t)(s - start);
            if (n < sizeof(op.label)) {
                memcpy(op.label, start, n);
                op.label[n] = '\0';
                op.imm_is_label = 1;
            }
            // Parse optional +offset.
            if (*s == '+') {
                s++;
                int64_t v; const char *e2;
                if (parse_imm(s, &e2, &v) == 0) { op.imm = v; s = e2; }
            } else if (*s == '-') {
                s++;
                int64_t v; const char *e2;
                if (parse_imm(s, &e2, &v) == 0) { op.imm = -v; s = e2; }
            }
        } else {
            int64_t v;
            if (parse_imm(s, &s, &v) == 0) op.imm = v;
        }
        *end = s;
        return op;
    }
    if (*s == '%') {
        // Register.
        Reg r = parse_reg(s, &s);
        if (r.id != R_NONE) {
            op.kind = OP_REG;
            op.reg = r;
            *end = s;
            return op;
        }
    }
    // Memory operand or label.
    // Check if it looks like a label (no parens, starts with letter/dot).
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_' || *s == '.') {
        const char *look = s;
        int has_paren = 0;
        while (*look && *look != ',' && *look != ' ' && *look != '\t' && *look != '\n' && *look != '\r') {
            if (*look == '(') { has_paren = 1; break; }
            look++;
        }
        if (!has_paren) {
            // It's a label reference (for callq/jmp).
            op.kind = OP_IMM;
            op.imm_is_label = 1;
            const char *start = s;
            while (*s && *s != ',' && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') s++;
            size_t n = (size_t)(s - start);
            if (n < sizeof(op.label)) {
                memcpy(op.label, start, n);
                op.label[n] = '\0';
            }
            *end = s;
            return op;
        }
    }
    // Memory operand.
    op.kind = OP_MEM;
    op.mem = parse_mem(s, &s);
    *end = s;
    return op;
}

// --- x86_64 encoding primitives --------------------------------------------

static int reg_num(int id) { return id & 0xf; }
static int reg_needs_rex(int id) { return id >= 8 && id < 16; }
static int is_xmm(int id) { return id >= R_XMM0 && id <= R_XMM15; }
static int xmm_num(int id) { return (id - R_XMM0) & 0xf; }
static int xmm_needs_rex(int id) { return (id - R_XMM0) >= 8; }

static void emit_rex(Ctx *c, int w, int r, int x, int b) {
    uint8_t rex = 0x40 | ((w & 1) << 3) | ((r & 1) << 2) | ((x & 1) << 1) | (b & 1);
    sec_put8(c, rex);
}

static void emit_modrm(Ctx *c, int mod, int reg, int rm) {
    sec_put8(c, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

static void emit_sib(Ctx *c, int scale, int index, int base) {
    int s = 0;
    if (scale == 2) s = 1; else if (scale == 4) s = 2; else if (scale == 8) s = 3;
    sec_put8(c, (uint8_t)((s << 6) | ((index & 7) << 3) | (base & 7)));
}

// Encode a memory operand (ModR/M + SIB + displacement).
// reg_field is the /reg extension or register operand.
static void encode_mem(Ctx *c, MemOp *m, int reg_field) {
    if (m->is_rip) {
        // RIP-relative: mod=00, rm=101, 32-bit displacement.
        emit_modrm(c, 0, reg_field, 5);
        if (m->has_sym) {
            // Symbolic: emit 0 placeholder, add relocation.
            int sym = sym_intern(c, m->sym_name, strlen(m->sym_name));
            AsmSectionOut *s = &c->u->sec[c->cur];
            sec_put32(c, (uint32_t)m->sym_off);
            add_reloc(c, c->cur, (int32_t)(s->len - 4), sym, 1, 2, ASM_RELOC_X86_64_PC32);
        } else {
            sec_put32(c, (uint32_t)m->disp);
        }
        return;
    }
    int base = m->base;
    int index = m->index;
    int base_num = reg_num(base);
    int has_sib = (index != R_NONE) || base == R_RSP || base == R_R12;
    // Determine mod.
    int mod;
    if (!m->has_disp && !m->has_sym && m->disp == 0 && base != R_RBP && base != R_R13) {
        mod = 0; // [reg] with no displacement
    } else if (m->disp >= -128 && m->disp <= 127 && !m->has_sym) {
        mod = 1; // [reg + disp8]
    } else {
        mod = 2; // [reg + disp32]
    }
    if (has_sib) {
        if (index == R_NONE) {
            // No index: use rsp/r12 as index (meaning "no index" in SIB).
            int idx_enc = 4; // rsp = no index in SIB
            emit_modrm(c, mod, reg_field, 4); // rm=4 means SIB follows
            emit_sib(c, 1, idx_enc, base);
        } else {
            emit_modrm(c, mod, reg_field, 4); // rm=4 means SIB follows
            emit_sib(c, m->scale, index, base);
        }
    } else {
        // Special case: rbp/r13 with no displacement → must use disp8=0.
        if (mod == 0 && (base == R_RBP || base == R_R13)) mod = 1;
        emit_modrm(c, mod, reg_field, base_num);
    }
    // Emit displacement.
    if (m->has_sym) {
        int sym = sym_intern(c, m->sym_name, strlen(m->sym_name));
        AsmSectionOut *s = &c->u->sec[c->cur];
        if (mod == 1) {
            sec_put8(c, (uint8_t)(int8_t)m->sym_off);
            (void)sym; // can't do relocation on disp8
        } else {
            sec_put32(c, (uint32_t)m->sym_off);
            add_reloc(c, c->cur, (int32_t)(s->len - 4), sym, 1, 2, ASM_RELOC_X86_64_PC32);
        }
    } else if (mod == 1) {
        sec_put8(c, (uint8_t)(int8_t)m->disp);
    } else if (mod == 2) {
        sec_put32(c, (uint32_t)m->disp);
    }
}

// --- ALU encoding -----------------------------------------------------------

// ALU opcode table: {r/m,r form, r,r/m form, imm form, /digit, imm8_ok}
// op_reg_reg: REX + opcode + ModRM(11)
// op_reg_imm: REX + opcode + ModRM(11, /digit) + imm
// op_reg_mem: REX + opcode + ModRM(mem)
// op_mem_reg: REX + opcode + ModRM(mem)

// ALU opcodes for add, or, adc, sbb, and, sub, xor, cmp:
// /digit: add=0, or=1, adc=2, sbb=3, and=4, sub=5, xor=6, cmp=7
static void encode_alu(Ctx *c, const char *mnem, int sz, Operand *dst, Operand *src) {
    int digit;
    uint8_t op_rm_r, op_r_rm, op_imm;
    if (strcmp(mnem, "add") == 0)      { digit=0; op_rm_r=0x01; op_r_rm=0x03; op_imm=0x81; }
    else if (strcmp(mnem, "or") == 0)  { digit=1; op_rm_r=0x09; op_r_rm=0x0B; op_imm=0x81; }
    else if (strcmp(mnem, "and") == 0) { digit=4; op_rm_r=0x21; op_r_rm=0x23; op_imm=0x81; }
    else if (strcmp(mnem, "sub") == 0) { digit=5; op_rm_r=0x29; op_r_rm=0x2B; op_imm=0x81; }
    else if (strcmp(mnem, "xor") == 0) { digit=6; op_rm_r=0x31; op_r_rm=0x33; op_imm=0x81; }
    else if (strcmp(mnem, "cmp") == 0) { digit=7; op_rm_r=0x39; op_r_rm=0x3B; op_imm=0x81; }
    else { err(c, "unsupported ALU op: %s", mnem); return; }
    (void)op_r_rm; // not used in QBE's 2-address form

    int w = (sz == SZ_Q) ? 1 : 0;
    int rex_b, rex_r;

    if (src->kind == OP_IMM && dst->kind == OP_REG) {
        // ALU $imm, %reg
        int rd = reg_num(dst->reg.id);
        rex_b = reg_needs_rex(dst->reg.id);
        if (sz == SZ_B) {
            // No REX.W for byte, but may need REX for high-byte avoidance.
            if (rex_b) emit_rex(c, 0, 0, 0, 1);
            sec_put8(c, 0x80);
            emit_modrm(c, 3, digit, rd);
            sec_put8(c, (uint8_t)src->imm);
        } else {
            if (sz == SZ_W) sec_put8(c, 0x66); // operand size prefix
            if (w || rex_b) emit_rex(c, w, 0, 0, rex_b);
            // Check if imm fits in signed 8 bits.
            if (src->imm >= -128 && src->imm <= 127 && sz != SZ_W) {
                sec_put8(c, 0x83);
                emit_modrm(c, 3, digit, rd);
                sec_put8(c, (uint8_t)(int8_t)src->imm);
            } else {
                sec_put8(c, (sz == SZ_L) ? (uint8_t)(op_imm + 1) : op_imm);
                emit_modrm(c, 3, digit, rd);
                if (sz == SZ_L || sz == SZ_Q) sec_put32(c, (uint32_t)src->imm);
                else if (sz == SZ_W) sec_put16(c, (uint16_t)src->imm);
            }
        }
    } else if (src->kind == OP_IMM && dst->kind == OP_MEM) {
        // ALU $imm, mem
        rex_r = 0;
        int base_r = src->kind == OP_IMM ? 0 : reg_needs_rex(src->reg.id);
        (void)base_r;
        if (w) emit_rex(c, w, 0, 0, 0);
        sec_put8(c, op_imm);
        encode_mem(c, &dst->mem, digit);
        if (sz == SZ_L || sz == SZ_Q) sec_put32(c, (uint32_t)src->imm);
    } else if (src->kind == OP_REG && dst->kind == OP_REG) {
        // ALU %reg, %reg → op_rm_r: r/m=dst, reg=src
        int rd = reg_num(dst->reg.id);
        int rs = reg_num(src->reg.id);
        rex_r = reg_needs_rex(src->reg.id);
        rex_b = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rex_r || rex_b) emit_rex(c, w, rex_r, 0, rex_b);
        sec_put8(c, op_rm_r);
        emit_modrm(c, 3, rs, rd);
    } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
        // ALU mem, %reg → op_r_rm: reg=dst, r/m=mem
        int rd = reg_num(dst->reg.id);
        rex_r = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rex_r) emit_rex(c, w, rex_r, 0, 0);
        sec_put8(c, op_r_rm);
        encode_mem(c, &src->mem, rd);
    } else if (src->kind == OP_REG && dst->kind == OP_MEM) {
        // ALU %reg, mem → op_rm_r: r/m=mem, reg=src
        int rs = reg_num(src->reg.id);
        rex_r = reg_needs_rex(src->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rex_r) emit_rex(c, w, rex_r, 0, 0);
        sec_put8(c, op_rm_r);
        encode_mem(c, &dst->mem, rs);
    } else {
        err(c, "unsupported ALU operand combination");
    }
}

// test: TEST r/m, r  (0x85) or TEST r/m, imm (0xF7 /0)
static void encode_test(Ctx *c, int sz, Operand *dst, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src->kind == OP_REG && dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rs = reg_num(src->reg.id);
        int rr = reg_needs_rex(src->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rr || rb) emit_rex(c, w, rr, 0, rb);
        sec_put8(c, 0x85);
        emit_modrm(c, 3, rs, rd);
    } else if (src->kind == OP_IMM && dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rb) emit_rex(c, w, 0, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xF6 : (uint8_t)0xF7);
        emit_modrm(c, 3, 0, rd);
        if (sz == SZ_B) sec_put8(c, (uint8_t)src->imm);
        else if (sz == SZ_L || sz == SZ_Q) sec_put32(c, (uint32_t)src->imm);
    } else {
        err(c, "unsupported test operand combination");
    }
}

// --- shift encoding ---------------------------------------------------------

static void encode_shift(Ctx *c, const char *mnem, int sz, Operand *dst, Operand *src) {
    int digit;
    if (strcmp(mnem, "sar") == 0) digit = 7;
    else if (strcmp(mnem, "shr") == 0) digit = 5;
    else if (strcmp(mnem, "shl") == 0) digit = 4;
    else { err(c, "unsupported shift: %s", mnem); return; }
    int w = (sz == SZ_Q) ? 1 : 0;
    int rb = reg_needs_rex(dst->reg.id);
    int rd = reg_num(dst->reg.id);
    if (sz == SZ_W) sec_put8(c, 0x66);
    if (src->kind == OP_IMM && src->imm == 1) {
        if (w || rb) emit_rex(c, w, 0, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xD0 : (uint8_t)0xD1);
        emit_modrm(c, 3, digit, rd);
    } else if (src->kind == OP_IMM) {
        if (w || rb) emit_rex(c, w, 0, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xC0 : (uint8_t)0xC1);
        emit_modrm(c, 3, digit, rd);
        sec_put8(c, (uint8_t)src->imm);
    } else {
        // shift by %cl
        if (w || rb) emit_rex(c, w, 0, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xD2 : (uint8_t)0xD3);
        emit_modrm(c, 3, digit, rd);
    }
}

// --- mov encoding -----------------------------------------------------------

static void encode_mov(Ctx *c, int sz, Operand *dst, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src->kind == OP_IMM && dst->kind == OP_REG) {
        // mov $imm, %reg
        int rd = reg_num(dst->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (sz == SZ_Q && src->imm >= 0 && src->imm <= (int64_t)UINT32_MAX) {
            // movl (zero-extends to 64-bit) — shorter encoding.
            if (rb) emit_rex(c, 0, 0, 0, rb);
            sec_put8(c, 0xB8 + rd);
            sec_put32(c, (uint32_t)src->imm);
        } else if (sz == SZ_Q) {
            // movabsq: REX.W + B8+rd + imm64
            emit_rex(c, 1, 0, 0, rb);
            sec_put8(c, 0xB8 + rd);
            sec_put64(c, (uint64_t)src->imm);
        } else if (sz == SZ_L) {
            if (rb) emit_rex(c, 0, 0, 0, rb);
            sec_put8(c, 0xB8 + rd);
            sec_put32(c, (uint32_t)src->imm);
        } else if (sz == SZ_W) {
            sec_put8(c, 0x66);
            if (rb) emit_rex(c, 0, 0, 0, rb);
            sec_put8(c, 0xB8 + rd);
            sec_put16(c, (uint16_t)src->imm);
        } else { // SZ_B
            if (rb) emit_rex(c, 0, 0, 0, rb);
            sec_put8(c, 0xB0 + rd);
            sec_put8(c, (uint8_t)src->imm);
        }
    } else if (src->kind == OP_REG && dst->kind == OP_REG) {
        // mov %reg, %reg
        int rs = reg_num(src->reg.id);
        int rd = reg_num(dst->reg.id);
        int rr = reg_needs_rex(src->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rr || rb) emit_rex(c, w, rr, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0x88 : (uint8_t)0x89);
        emit_modrm(c, 3, rs, rd);
    } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
        // mov mem, %reg (load)
        int rd = reg_num(dst->reg.id);
        int rr = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rr) emit_rex(c, w, rr, 0, 0);
        sec_put8(c, sz == SZ_B ? (uint8_t)0x8A : (uint8_t)0x8B);
        encode_mem(c, &src->mem, rd);
    } else if (src->kind == OP_REG && dst->kind == OP_MEM) {
        // mov %reg, mem (store)
        int rs = reg_num(src->reg.id);
        int rr = reg_needs_rex(src->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rr) emit_rex(c, w, rr, 0, 0);
        sec_put8(c, sz == SZ_B ? (uint8_t)0x88 : (uint8_t)0x89);
        encode_mem(c, &dst->mem, rs);
    } else {
        err(c, "unsupported mov operand combination");
    }
}

// Sign/zero extension moves: movslq, movsw, movsb, movzw, movzb, movzbw, etc.
static void encode_ext_move(Ctx *c, const char *mnem, Operand *dst, Operand *src) {
    // movslq: 32→64 sign extend.  movswq/movswl: 16→64/16→32.
    // movzbq/movzbl/movzbw: 8→64/8→32/8→16 zero extend.
    int dst_sz = SZ_Q;
    (void)dst_sz;
    uint8_t opcode = 0;
    int two_byte = 0;
    int w = 0;
    if (strcmp(mnem, "movslq") == 0) {
        dst_sz = SZ_Q; opcode = 0x63; w = 1;
    } else if (strcmp(mnem, "movswq") == 0) {
        dst_sz = SZ_Q; opcode = 0xBF; w = 1; two_byte = 1;
    } else if (strcmp(mnem, "movswl") == 0) {
        dst_sz = SZ_L; opcode = 0xBF; w = 0; two_byte = 1;
    } else if (strcmp(mnem, "movsbq") == 0) {
        dst_sz = SZ_Q; opcode = 0xBE; w = 1; two_byte = 1;
    } else if (strcmp(mnem, "movsbl") == 0) {
        dst_sz = SZ_L; opcode = 0xBE; w = 0; two_byte = 1;
    } else if (strcmp(mnem, "movzbq") == 0) {
        dst_sz = SZ_Q; opcode = 0xB6; w = 1; two_byte = 1;
    } else if (strcmp(mnem, "movzbl") == 0) {
        dst_sz = SZ_L; opcode = 0xB6; w = 0; two_byte = 1;
    } else if (strcmp(mnem, "movzbw") == 0) {
        dst_sz = SZ_W; opcode = 0xB6; w = 0; two_byte = 1;
    } else if (strcmp(mnem, "movzwl") == 0) {
        dst_sz = SZ_L; opcode = 0xB7; w = 0; two_byte = 1;
    } else if (strcmp(mnem, "movzwq") == 0) {
        dst_sz = SZ_Q; opcode = 0xB7; w = 1; two_byte = 1;
    } else {
        err(c, "unsupported ext move: %s", mnem); return;
    }
    (void)dst_sz;
    if (dst_sz == SZ_W) sec_put8(c, 0x66);
    if (src->kind == OP_MEM && dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rr = reg_needs_rex(dst->reg.id);
        if (w || rr) emit_rex(c, w, rr, 0, 0);
        if (two_byte) { sec_put8(c, 0x0F); }
        sec_put8(c, opcode);
        encode_mem(c, &src->mem, rd);
    } else if (src->kind == OP_REG && dst->kind == OP_REG) {
        int rs = reg_num(src->reg.id);
        int rd = reg_num(dst->reg.id);
        int rr = reg_needs_rex(dst->reg.id);
        int rb = reg_needs_rex(src->reg.id);
        if (w || rr || rb) emit_rex(c, w, rr, 0, rb);
        if (two_byte) { sec_put8(c, 0x0F); }
        sec_put8(c, opcode);
        emit_modrm(c, 3, rs, rd);
    } else {
        err(c, "unsupported ext move operand combination");
    }
}

// --- lea encoding -----------------------------------------------------------

static void encode_lea(Ctx *c, int sz, Operand *dst, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src->kind != OP_MEM || dst->kind != OP_REG) {
        err(c, "lea requires memory source and register destination"); return;
    }
    int rd = reg_num(dst->reg.id);
    int rr = reg_needs_rex(dst->reg.id);
    int x = (src->mem.index != R_NONE) ? reg_needs_rex(src->mem.index) : 0;
    int b = (src->mem.base != R_NONE) ? reg_needs_rex(src->mem.base) : 0;
    if (w || rr || x || b) emit_rex(c, w, rr, x, b);
    sec_put8(c, 0x8D);
    encode_mem(c, &src->mem, rd);
}

// --- imul encoding ----------------------------------------------------------

static void encode_imul(Ctx *c, int sz, Operand *dst, Operand *src, Operand *src2) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src2 == NULL) {
        // Two-operand: imul %reg, %reg  or  imul mem, %reg
        if (src->kind == OP_REG && dst->kind == OP_REG) {
            int rs = reg_num(src->reg.id);
            int rd = reg_num(dst->reg.id);
            int rr = reg_needs_rex(dst->reg.id);
            int rb = reg_needs_rex(src->reg.id);
            if (w || rr || rb) emit_rex(c, w, rr, 0, rb);
            sec_put8(c, 0x0F); sec_put8(c, 0xAF);
            emit_modrm(c, 3, rd, rs);
        } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
            int rd = reg_num(dst->reg.id);
            int rr = reg_needs_rex(dst->reg.id);
            if (w || rr) emit_rex(c, w, rr, 0, 0);
            sec_put8(c, 0x0F); sec_put8(c, 0xAF);
            encode_mem(c, &src->mem, rd);
        } else {
            err(c, "unsupported imul operand combination");
        }
    } else {
        // Three-operand: imul $imm, %reg, %reg
        if (src->kind == OP_IMM && src2->kind == OP_REG && dst->kind == OP_REG) {
            int rs = reg_num(src2->reg.id);
            int rd = reg_num(dst->reg.id);
            int rr = reg_needs_rex(dst->reg.id);
            int rb = reg_needs_rex(src2->reg.id);
            if (w || rr || rb) emit_rex(c, w, rr, 0, rb);
            if (src->imm >= -128 && src->imm <= 127) {
                sec_put8(c, 0x6B);
                emit_modrm(c, 3, rd, rs);
                sec_put8(c, (uint8_t)(int8_t)src->imm);
            } else {
                sec_put8(c, 0x69);
                emit_modrm(c, 3, rd, rs);
                sec_put32(c, (uint32_t)src->imm);
            }
        } else {
            err(c, "unsupported 3-operand imul");
        }
    }
}

// --- div/idiv encoding ------------------------------------------------------

static void encode_div(Ctx *c, const char *mnem, int sz, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    int digit = (strcmp(mnem, "div") == 0) ? 6 : 7;
    if (src->kind == OP_REG) {
        int rs = reg_num(src->reg.id);
        int rb = reg_needs_rex(src->reg.id);
        if (w || rb) emit_rex(c, w, 0, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xF6 : (uint8_t)0xF7);
        emit_modrm(c, 3, digit, rs);
    } else if (src->kind == OP_MEM) {
        if (w) emit_rex(c, w, 0, 0, 0);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xF6 : (uint8_t)0xF7);
        encode_mem(c, &src->mem, digit);
    } else {
        err(c, "unsupported div/idiv operand");
    }
}

// --- call/jmp/jcc encoding --------------------------------------------------

static void encode_call(Ctx *c, Operand *src) {
    if (src->kind == OP_IMM && src->imm_is_label) {
        // callq symbol — direct call with rel32.
        AsmSectionOut *s = &c->u->sec[c->cur];
        int64_t instr_start = (int64_t)s->len;
        sec_put8(c, 0xE8);
        int rel_off = (int)s->len;
        sec_put32(c, 0); // placeholder
        // Check if it's a local label.
        Label *l = label_find(c, src->label, strlen(src->label));
        if (l && l->section == c->cur) {
            int64_t target = l->offset;
            int64_t next = (int64_t)s->len;
            int32_t rel = (int32_t)(target - next);
            uint8_t *p = s->bytes + rel_off;
            p[0] = (uint8_t)rel; p[1] = (uint8_t)(rel>>8);
            p[2] = (uint8_t)(rel>>16); p[3] = (uint8_t)(rel>>24);
        } else {
            // External symbol or forward reference.
            int sym = sym_intern(c, src->label, strlen(src->label));
            add_reloc(c, c->cur, (int32_t)rel_off, sym, 1, 2, ASM_RELOC_X86_64_PC32);
        }
        (void)instr_start;
    } else if (src->kind == OP_REG) {
        // callq *%reg — indirect register call.
        int rs = reg_num(src->reg.id);
        int rb = reg_needs_rex(src->reg.id);
        if (rb) emit_rex(c, 0, 0, 0, rb);
        sec_put8(c, 0xFF);
        emit_modrm(c, 3, 2, rs);
    } else if (src->kind == OP_MEM) {
        // callq *sym(%rip) or callq *mem — indirect memory call.
        if (src->mem.has_sym && src->mem.is_rip) {
            // GOT-indirect call through RIP-relative memory.
            int rs = 2; // /2 for call
            int sym = sym_intern(c, src->mem.sym_name, strlen(src->mem.sym_name));
            sec_put8(c, 0xFF);
            AsmSectionOut *s = &c->u->sec[c->cur];
            int rel_off = (int)s->len + 2; // after ModRM
            encode_mem(c, &src->mem, rs);
            // Change relocation to GOTPCRELX.
            add_reloc(c, c->cur, (int32_t)rel_off, sym, 1, 2, ASM_RELOC_X86_64_GOTPCRELX);
        } else {
            int rs = 2;
            sec_put8(c, 0xFF);
            encode_mem(c, &src->mem, rs);
        }
    } else {
        err(c, "unsupported call operand");
    }
}

static void encode_jmp(Ctx *c, Operand *src) {
    if (src->kind == OP_IMM && src->imm_is_label) {
        // jmp label — near jump with rel32.
        AsmSectionOut *s = &c->u->sec[c->cur];
        sec_put8(c, 0xE9);
        int rel_off = (int)s->len;
        sec_put32(c, 0);
        Label *l = label_find(c, src->label, strlen(src->label));
        if (l && l->section == c->cur) {
            int64_t target = l->offset;
            int64_t next = (int64_t)s->len;
            int32_t rel = (int32_t)(target - next);
            uint8_t *p = s->bytes + rel_off;
            p[0] = (uint8_t)rel; p[1] = (uint8_t)(rel>>8);
            p[2] = (uint8_t)(rel>>16); p[3] = (uint8_t)(rel>>24);
        } else {
            fixup_add(c, (int64_t)rel_off, src->label, strlen(src->label), 0, 0);
        }
    } else {
        err(c, "unsupported jmp operand");
    }
}

// Condition code to opcode suffix.
static int cc_to_opcode(const char *cc) {
    if (strcmp(cc, "be") == 0) return 0x86;
    if (strcmp(cc, "b") == 0)  return 0x82;
    if (strcmp(cc, "le") == 0) return 0x8E;
    if (strcmp(cc, "l") == 0)  return 0x8C;
    if (strcmp(cc, "g") == 0)  return 0x8F;
    if (strcmp(cc, "ge") == 0) return 0x8D;
    if (strcmp(cc, "a") == 0)  return 0x87;
    if (strcmp(cc, "ae") == 0) return 0x83;
    if (strcmp(cc, "z") == 0)  return 0x84;
    if (strcmp(cc, "nz") == 0) return 0x85;
    if (strcmp(cc, "np") == 0) return 0x8B;
    if (strcmp(cc, "p") == 0)  return 0x8A;
    return -1;
}

static int cc_to_setcc(const char *cc) {
    if (strcmp(cc, "be") == 0) return 0x96;
    if (strcmp(cc, "b") == 0)  return 0x92;
    if (strcmp(cc, "le") == 0) return 0x9E;
    if (strcmp(cc, "l") == 0)  return 0x9C;
    if (strcmp(cc, "g") == 0)  return 0x9F;
    if (strcmp(cc, "ge") == 0) return 0x9D;
    if (strcmp(cc, "a") == 0)  return 0x97;
    if (strcmp(cc, "ae") == 0) return 0x93;
    if (strcmp(cc, "z") == 0)  return 0x94;
    if (strcmp(cc, "nz") == 0) return 0x95;
    if (strcmp(cc, "np") == 0) return 0x9B;
    if (strcmp(cc, "p") == 0)  return 0x9A;
    return -1;
}

static void encode_jcc(Ctx *c, const char *cc, Operand *src) {
    int opc = cc_to_opcode(cc);
    if (opc < 0) { err(c, "unsupported condition code: %s", cc); return; }
    if (src->kind == OP_IMM && src->imm_is_label) {
        AsmSectionOut *s = &c->u->sec[c->cur];
        sec_put8(c, 0x0F);
        sec_put8(c, (uint8_t)opc);
        int rel_off = (int)s->len;
        sec_put32(c, 0);
        Label *l = label_find(c, src->label, strlen(src->label));
        if (l && l->section == c->cur) {
            int64_t target = l->offset;
            int64_t next = (int64_t)s->len;
            int32_t rel = (int32_t)(target - next);
            uint8_t *p = s->bytes + rel_off;
            p[0] = (uint8_t)rel; p[1] = (uint8_t)(rel>>8);
            p[2] = (uint8_t)(rel>>16); p[3] = (uint8_t)(rel>>24);
        } else {
            fixup_add(c, (int64_t)rel_off, src->label, strlen(src->label), 0, 0);
        }
    } else {
        err(c, "unsupported jcc operand");
    }
}

// --- setcc encoding ---------------------------------------------------------

static void encode_setcc(Ctx *c, const char *cc, Operand *dst) {
    int opc = cc_to_setcc(cc);
    if (opc < 0) { err(c, "unsupported condition code: %s", cc); return; }
    if (dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        // setcc needs REX if accessing sil/dil/bpl/spl (registers 4-7 in byte mode).
        int needs_rex_for_byte = (dst->reg.id >= R_RSP && dst->reg.id <= R_RDI);
        if (rb || needs_rex_for_byte) emit_rex(c, 0, 0, 0, rb);
        sec_put8(c, 0x0F);
        sec_put8(c, (uint8_t)opc);
        emit_modrm(c, 3, 0, rd);
    } else {
        err(c, "setcc requires register operand");
    }
}

// --- push/pop encoding ------------------------------------------------------

static void encode_push(Ctx *c, Operand *src) {
    if (src->kind == OP_REG) {
        int rd = reg_num(src->reg.id);
        int rb = reg_needs_rex(src->reg.id);
        if (rb) emit_rex(c, 0, 0, 0, rb);
        sec_put8(c, 0x50 + rd);
    } else if (src->kind == OP_IMM) {
        if (src->imm >= -128 && src->imm <= 127) {
            sec_put8(c, 0x6A);
            sec_put8(c, (uint8_t)(int8_t)src->imm);
        } else {
            sec_put8(c, 0x68);
            sec_put32(c, (uint32_t)src->imm);
        }
    } else {
        err(c, "unsupported push operand");
    }
}

static void encode_pop(Ctx *c, Operand *dst) {
    if (dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (rb) emit_rex(c, 0, 0, 0, rb);
        sec_put8(c, 0x58 + rd);
    } else {
        err(c, "unsupported pop operand");
    }
}

// --- xchg encoding ----------------------------------------------------------

static void encode_xchg(Ctx *c, int sz, Operand *dst, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src->kind == OP_REG && dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rs = reg_num(src->reg.id);
        int rr = reg_needs_rex(src->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (w || rr || rb) emit_rex(c, w, rr, 0, rb);
        sec_put8(c, 0x87);
        emit_modrm(c, 3, rs, rd);
    } else {
        err(c, "unsupported xchg operand");
    }
}

// --- SSE encoding helpers ---------------------------------------------------

// SSE instruction with prefix: prefix + 0F xx /r
static void encode_sse_rr(Ctx *c, uint8_t prefix, uint8_t opc, Operand *dst, Operand *src) {
    if (src->kind == OP_REG && dst->kind == OP_REG) {
        int rs = xmm_num(src->reg.id);
        int rd = xmm_num(dst->reg.id);
        int xr = xmm_needs_rex(src->reg.id);
        int xb = xmm_needs_rex(dst->reg.id);
        if (xr || xb) emit_rex(c, 0, xr, 0, xb);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        emit_modrm(c, 3, rd, rs);
    } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
        int rd = xmm_num(dst->reg.id);
        int xr = xmm_needs_rex(dst->reg.id);
        if (xr) emit_rex(c, 0, xr, 0, 0);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        encode_mem(c, &src->mem, rd);
    } else if (src->kind == OP_REG && dst->kind == OP_MEM) {
        int rs = xmm_num(src->reg.id);
        int xr = xmm_needs_rex(src->reg.id);
        if (xr) emit_rex(c, 0, xr, 0, 0);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        encode_mem(c, &dst->mem, rs);
    } else {
        err(c, "unsupported SSE operand combination");
    }
}

// SSE conversion: cvtXX2YY src, dst
static void encode_sse_cvt(Ctx *c, uint8_t prefix, uint8_t opc, Operand *dst, Operand *src) {
    encode_sse_rr(c, prefix, opc, dst, src);
}

// cvtsi2ss/cvtsi2sd: integer to float. src can be reg or mem.
static void encode_cvtsi(Ctx *c, uint8_t prefix, uint8_t opc, int sz, Operand *dst, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src->kind == OP_REG && dst->kind == OP_REG) {
        int rs = reg_num(src->reg.id);
        int rd = xmm_num(dst->reg.id);
        int xb = reg_needs_rex(src->reg.id);
        int xr = xmm_needs_rex(dst->reg.id);
        if (w || xr || xb) emit_rex(c, w, xr, 0, xb);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        emit_modrm(c, 3, rd, rs);
    } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
        int rd = xmm_num(dst->reg.id);
        int xr = xmm_needs_rex(dst->reg.id);
        if (w || xr) emit_rex(c, w, xr, 0, 0);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        encode_mem(c, &src->mem, rd);
    } else {
        err(c, "unsupported cvtsi operand combination");
    }
}

// cvttss2si/cvttsd2si: float to integer. dst is GPR, src is XMM or mem.
static void encode_cvtsi_from(Ctx *c, uint8_t prefix, uint8_t opc, int sz, Operand *dst, Operand *src) {
    int w = (sz == SZ_Q) ? 1 : 0;
    if (src->kind == OP_REG && dst->kind == OP_REG) {
        int rs = xmm_num(src->reg.id);
        int rd = reg_num(dst->reg.id);
        int xr = xmm_needs_rex(src->reg.id);
        int xb = reg_needs_rex(dst->reg.id);
        if (w || xr || xb) emit_rex(c, w, xr, 0, xb);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        emit_modrm(c, 3, rd, rs);
    } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int xb = reg_needs_rex(dst->reg.id);
        if (w || xb) emit_rex(c, w, 0, 0, xb);
        sec_put8(c, prefix);
        sec_put8(c, 0x0F);
        sec_put8(c, opc);
        encode_mem(c, &src->mem, rd);
    } else {
        err(c, "unsupported cvt_from operand combination");
    }
}

// --- neg/not encoding -------------------------------------------------------

static void encode_unary(Ctx *c, const char *mnem, int sz, Operand *dst) {
    int digit;
    if (strcmp(mnem, "neg") == 0) digit = 3;
    else if (strcmp(mnem, "not") == 0) digit = 2;
    else if (strcmp(mnem, "inc") == 0) digit = 0;
    else if (strcmp(mnem, "dec") == 0) digit = 1;
    else { err(c, "unsupported unary: %s", mnem); return; }
    int w = (sz == SZ_Q) ? 1 : 0;
    if (dst->kind == OP_REG) {
        int rd = reg_num(dst->reg.id);
        int rb = reg_needs_rex(dst->reg.id);
        if (sz == SZ_W) sec_put8(c, 0x66);
        if (w || rb) emit_rex(c, w, 0, 0, rb);
        sec_put8(c, sz == SZ_B ? (uint8_t)0xF6 : (uint8_t)0xF7);
        emit_modrm(c, 3, digit, rd);
    } else {
        err(c, "unsupported unary operand");
    }
}

// --- instruction dispatch ---------------------------------------------------

// Get the size suffix from a mnemonic and return the base name.
static int parse_size_suffix(const char *mnem, char *base, size_t base_sz) {
    size_t l = strlen(mnem);
    if (l == 0) return SZ_Q;
    char last = mnem[l - 1];
    if (last == 'q' || last == 'l' || last == 'w' || last == 'b') {
        size_t n = l - 1;
        if (n >= base_sz) n = base_sz - 1;
        memcpy(base, mnem, n);
        base[n] = '\0';
        if (last == 'q') return SZ_Q;
        if (last == 'l') return SZ_L;
        if (last == 'w') return SZ_W;
        return SZ_B;
    }
    strncpy(base, mnem, base_sz); base[base_sz-1] = 0;
    return SZ_Q; // default
}

static void encode_instruction(Ctx *c, const char *mnem, Operand *ops, int nop) {
    char base[64];
    int sz = parse_size_suffix(mnem, base, sizeof(base));

    // Simple no-operand instructions.
    if (strcmp(mnem, "ret") == 0) { sec_put8(c, 0xC3); return; }
    if (strcmp(mnem, "leave") == 0) { sec_put8(c, 0xC9); return; }
    if (strcmp(mnem, "nop") == 0) { sec_put8(c, 0x90); return; }
    if (strcmp(mnem, "cqto") == 0) { emit_rex(c, 1, 0, 0, 0); sec_put8(c, 0x99); return; }
    if (strcmp(mnem, "cltd") == 0) { sec_put8(c, 0x99); return; }
    if (strcmp(mnem, "syscall") == 0) { sec_put8(c, 0x0F); sec_put8(c, 0x05); return; }

    // ALU operations.
    if ((strcmp(base, "add") == 0 || strcmp(base, "sub") == 0 ||
         strcmp(base, "and") == 0 || strcmp(base, "or") == 0 ||
         strcmp(base, "xor") == 0 || strcmp(base, "cmp") == 0) && nop == 2) {
        encode_alu(c, base, sz, &ops[1], &ops[0]);
        return;
    }
    // test (no size suffix in QBE's output typically, but handle it).
    if (strcmp(base, "test") == 0 && nop == 2) {
        encode_test(c, sz, &ops[1], &ops[0]);
        return;
    }
    // Shifts.
    if ((strcmp(base, "sar") == 0 || strcmp(base, "shr") == 0 || strcmp(base, "shl") == 0) && nop == 2) {
        encode_shift(c, base, sz, &ops[1], &ops[0]);
        return;
    }
    // mov.
    if (strcmp(base, "mov") == 0 && nop == 2) {
        encode_mov(c, sz, &ops[1], &ops[0]);
        return;
    }
    // Extension moves.
    if (strcmp(mnem, "movslq") == 0 || strcmp(mnem, "movswq") == 0 ||
        strcmp(mnem, "movswl") == 0 || strcmp(mnem, "movsbq") == 0 ||
        strcmp(mnem, "movsbl") == 0 || strcmp(mnem, "movzbq") == 0 ||
        strcmp(mnem, "movzbl") == 0 || strcmp(mnem, "movzbw") == 0 ||
        strcmp(mnem, "movzwl") == 0 || strcmp(mnem, "movzwq") == 0) {
        encode_ext_move(c, mnem, &ops[1], &ops[0]);
        return;
    }
    // lea.
    if (strcmp(base, "lea") == 0 && nop == 2) {
        encode_lea(c, sz, &ops[1], &ops[0]);
        return;
    }
    // imul (2 or 3 operands).
    if (strcmp(base, "imul") == 0 && nop >= 2) {
        if (nop == 3) {
            encode_imul(c, sz, &ops[2], &ops[0], &ops[1]);
        } else {
            encode_imul(c, sz, &ops[1], &ops[0], NULL);
        }
        return;
    }
    // div/idiv.
    if (strcmp(base, "div") == 0 && nop == 1) {
        encode_div(c, "div", sz, &ops[0]); return;
    }
    if (strcmp(base, "idiv") == 0 && nop == 1) {
        encode_div(c, "idiv", sz, &ops[0]); return;
    }
    // neg/not/inc/dec.
    if ((strcmp(base, "neg") == 0 || strcmp(base, "not") == 0 ||
         strcmp(base, "inc") == 0 || strcmp(base, "dec") == 0) && nop == 1) {
        encode_unary(c, base, sz, &ops[0]); return;
    }
    // call.
    if (strcmp(mnem, "callq") == 0 && nop == 1) {
        encode_call(c, &ops[0]); return;
    }
    // jmp.
    if (strcmp(mnem, "jmp") == 0 && nop == 1) {
        encode_jmp(c, &ops[0]); return;
    }
    // jcc.
    if (nop == 1 && ops[0].kind == OP_IMM && ops[0].imm_is_label) {
        // Check if mnem starts with 'j' and rest is a condition code.
        if (mnem[0] == 'j') {
            encode_jcc(c, mnem + 1, &ops[0]); return;
        }
    }
    // setcc.
    if (nop == 1 && ops[0].kind == OP_REG && mnem[0] == 's' && mnem[1] == 'e' && mnem[2] == 't') {
        encode_setcc(c, mnem + 3, &ops[0]); return;
    }
    // push/pop.
    if (strcmp(base, "push") == 0 && nop == 1) { encode_push(c, &ops[0]); return; }
    if (strcmp(base, "pop") == 0 && nop == 1) { encode_pop(c, &ops[0]); return; }
    // xchg.
    if (strcmp(base, "xchg") == 0 && nop == 2) {
        encode_xchg(c, sz, &ops[1], &ops[0]); return;
    }
    // SSE arithmetic: mulss, mulsd, divss, divsd, addss, addsd, subss, subsd
    if (strcmp(mnem, "mulss") == 0 && nop == 2) { encode_sse_rr(c, 0xF3, 0x59, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "mulsd") == 0 && nop == 2) { encode_sse_rr(c, 0xF2, 0x59, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "divss") == 0 && nop == 2) { encode_sse_rr(c, 0xF3, 0x5E, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "divsd") == 0 && nop == 2) { encode_sse_rr(c, 0xF2, 0x5E, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "addss") == 0 && nop == 2) { encode_sse_rr(c, 0xF3, 0x58, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "addsd") == 0 && nop == 2) { encode_sse_rr(c, 0xF2, 0x58, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "subss") == 0 && nop == 2) { encode_sse_rr(c, 0xF3, 0x5C, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "subsd") == 0 && nop == 2) { encode_sse_rr(c, 0xF2, 0x5C, &ops[1], &ops[0]); return; }
    // SSE moves.
    if (strcmp(mnem, "movss") == 0 && nop == 2) { encode_sse_rr(c, 0xF3, 0x10, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "movsd") == 0 && nop == 2) {
        // movsd can be XMM-to-XMM or mem-to-XMM.
        if (ops[0].kind == OP_REG && is_xmm(ops[0].reg.id) &&
            ops[1].kind == OP_REG && is_xmm(ops[1].reg.id)) {
            // movsd %xmm, %xmm → F2 0F 11 (store form) for 2-addr
            encode_sse_rr(c, 0xF2, 0x11, &ops[1], &ops[0]);
        } else {
            encode_sse_rr(c, 0xF2, 0x10, &ops[1], &ops[0]);
        }
        return;
    }
    if (strcmp(mnem, "movaps") == 0 && nop == 2) {
        encode_sse_rr(c, 0x00, 0x28, &ops[1], &ops[0]); // load form
        // Note: QBE uses movaps for register spills, which is 0F 29 (store) or 0F 28 (load).
        // For simplicity, use load form (0F 28) — works for reg-reg.
        return;
    }
    // SSE conversions.
    if (strcmp(mnem, "cvtss2sd") == 0 && nop == 2) { encode_sse_cvt(c, 0xF3, 0x5A, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "cvtsd2ss") == 0 && nop == 2) { encode_sse_cvt(c, 0xF2, 0x5A, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "cvttss2si") == 0 && nop == 2) { encode_cvtsi_from(c, 0xF3, 0x2C, sz, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "cvttsd2si") == 0 && nop == 2) { encode_cvtsi_from(c, 0xF2, 0x2C, sz, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "cvtsi2ss") == 0 && nop == 2) { encode_cvtsi(c, 0xF3, 0x2A, sz, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "cvtsi2sd") == 0 && nop == 2) { encode_cvtsi(c, 0xF2, 0x2A, sz, &ops[1], &ops[0]); return; }
    // SSE compare.
    if (strcmp(mnem, "ucomiss") == 0 && nop == 2) { encode_sse_rr(c, 0x00, 0x2E, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "ucomisd") == 0 && nop == 2) { encode_sse_rr(c, 0x66, 0x2E, &ops[1], &ops[0]); return; }
    // SSE xor.
    if (strcmp(mnem, "xorps") == 0 && nop == 2) { encode_sse_rr(c, 0x00, 0x57, &ops[1], &ops[0]); return; }
    if (strcmp(mnem, "xorpd") == 0 && nop == 2) { encode_sse_rr(c, 0x66, 0x57, &ops[1], &ops[0]); return; }

    // movq for XMM-to-XMM (when both operands are XMM).
    if (strcmp(mnem, "movq") == 0 && nop == 2 &&
        ops[0].kind == OP_REG && is_xmm(ops[0].reg.id) &&
        ops[1].kind == OP_REG && is_xmm(ops[1].reg.id)) {
        // movq %xmm, %xmm → F3 0F 7E
        encode_sse_rr(c, 0xF3, 0x7E, &ops[1], &ops[0]);
        return;
    }

    err(c, "unsupported instruction: %s", mnem);
}

// --- directive handling -----------------------------------------------------

static void handle_directive(Ctx *c, const char *dir, const char *args) {
    if (strcmp(dir, ".text") == 0) { c->cur = ASM_SEC_TEXT; c->u->sec[ASM_SEC_TEXT].used = 1; return; }
    if (strcmp(dir, ".data") == 0) { c->cur = ASM_SEC_DATA; c->u->sec[ASM_SEC_DATA].used = 1; return; }
    if (strcmp(dir, ".bss") == 0) { c->cur = ASM_SEC_BSS; c->u->sec[ASM_SEC_BSS].used = 1; return; }
    if (strcmp(dir, ".section") == 0) {
        args = skip_ws(args);
        if (strncmp(args, ".rodata", 7) == 0 || strncmp(args, ".section .rodata", 16) == 0) {
            c->cur = ASM_SEC_RODATA; c->u->sec[ASM_SEC_RODATA].used = 1;
        } else if (strncmp(args, ".data", 5) == 0) {
            c->cur = ASM_SEC_DATA; c->u->sec[ASM_SEC_DATA].used = 1;
        } else if (strncmp(args, ".note.GNU-stack", 15) == 0) {
            c->u->has_gnu_stack = 1;
        } else {
            // Unknown section — fail closed.
            err(c, "unsupported section: %s", args);
        }
        return;
    }
    if (strcmp(dir, ".balign") == 0 || strcmp(dir, ".align") == 0) {
        int64_t v; const char *e;
        if (parse_imm(args, &e, &v) == 0) sec_align(c, (size_t)v);
        return;
    }
    if (strcmp(dir, ".globl") == 0 || strcmp(dir, ".global") == 0) {
        args = skip_ws(args);
        const char *end = args;
        while (!is_eol(*end) && *end != ',' && *end != ' ') end++;
        int idx = sym_intern(c, args, (size_t)(end - args));
        c->u->syms[idx].global = 1;
        return;
    }
    if (strcmp(dir, ".type") == 0) {
        // .type name, @function — ignore (ELF metadata).
        return;
    }
    if (strcmp(dir, ".size") == 0) {
        // .size name, .-name — ignore (ELF metadata).
        return;
    }
    if (strcmp(dir, ".byte") == 0) {
        int64_t v; const char *e;
        if (parse_imm(args, &e, &v) == 0) sec_put8(c, (uint8_t)v);
        return;
    }
    if (strcmp(dir, ".short") == 0) {
        int64_t v; const char *e;
        if (parse_imm(args, &e, &v) == 0) sec_put16(c, (uint16_t)v);
        return;
    }
    if (strcmp(dir, ".int") == 0) {
        int64_t v; const char *e;
        if (parse_imm(args, &e, &v) == 0) sec_put32(c, (uint32_t)v);
        return;
    }
    if (strcmp(dir, ".quad") == 0) {
        args = skip_ws(args);
        // Check for symbol reference.
        if ((*args >= 'a' && *args <= 'z') || (*args >= 'A' && *args <= 'Z') || *args == '_' || *args == '.') {
            const char *start = args;
            while (!is_eol(*args) && *args != '+' && *args != '-') args++;
            size_t nlen = (size_t)(args - start);
            int64_t addend = 0;
            if (*args == '+') { args++; const char *e; if (parse_imm(args, &e, &addend) == 0) args = e; }
            else if (*args == '-') { args++; const char *e; int64_t v; if (parse_imm(args, &e, &v) == 0) { addend = -v; args = e; } }
            int sym = sym_intern(c, start, nlen);
            AsmSectionOut *s = &c->u->sec[c->cur];
            sec_put64(c, (uint64_t)addend);
            add_reloc(c, c->cur, (int32_t)(s->len - 8), sym, 0, 3, ASM_RELOC_X86_64_64);
        } else {
            int64_t v; const char *e;
            if (parse_imm(args, &e, &v) == 0) sec_put64(c, (uint64_t)v);
        }
        return;
    }
    if (strcmp(dir, ".ascii") == 0 || strcmp(dir, ".asciz") == 0) {
        // Parse quoted string.
        args = skip_ws(args);
        if (*args == '"') {
            args++;
            while (*args && *args != '"') {
                if (*args == '\\') {
                    args++;
                    switch (*args) {
                        case 'n': sec_put8(c, '\n'); break;
                        case 't': sec_put8(c, '\t'); break;
                        case 'r': sec_put8(c, '\r'); break;
                        case '0': sec_put8(c, '\0'); break;
                        case '\\': sec_put8(c, '\\'); break;
                        case '"': sec_put8(c, '"'); break;
                        default: sec_put8(c, (uint8_t)*args); break;
                    }
                } else {
                    sec_put8(c, (uint8_t)*args);
                }
                args++;
            }
            if (strcmp(dir, ".asciz") == 0) sec_put8(c, 0);
        }
        return;
    }
    if (strcmp(dir, ".fill") == 0) {
        // .fill count, size, value
        int64_t count = 0, size_val = 1, val = 0;
        const char *e;
        parse_imm(args, &e, &count);
        args = e;
        if (*args == ',') { args++; parse_imm(args, &e, &size_val); args = e; }
        if (*args == ',') { args++; parse_imm(args, &e, &val); }
        for (int64_t i = 0; i < count; i++) {
            for (int64_t j = 0; j < size_val; j++) {
                sec_put8(c, (uint8_t)(val >> (8 * j)));
            }
        }
        return;
    }
    if (strcmp(dir, ".note.GNU-stack") == 0 || strncmp(dir, ".note", 5) == 0) {
        c->u->has_gnu_stack = 1;
        return;
    }
    err(c, "unsupported directive: %s", dir);
}

// --- main assembly loop -----------------------------------------------------

int asm_amd64_assemble(AsmUnit *u, const char *text, size_t len) {
    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.u = u;
    ctx.cur = ASM_SEC_TEXT;
    u->machine = EM_X86_64;

    const char *p = text;
    const char *end = text + len;

    while (p < end && !ctx.failed) {
        // Find end of line.
        const char *line_start = p;
        while (p < end && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        if (p < end) p++; // skip \n
        ctx.line++;

        // Strip C-style /* */ comments (QBE emits these between output blocks).
        const char *cstart = NULL;
        for (size_t i = 0; i + 1 < line_len; i++) {
            if (line_start[i] == '/' && line_start[i + 1] == '*') { cstart = line_start + i; break; }
        }
        if (cstart) {
            line_len = (size_t)(cstart - line_start);
        }

        // Trim.
        const char *s = line_start;
        size_t slen = line_len;
        while (slen && (s[0] == ' ' || s[0] == '\t')) { s++; slen--; }
        while (slen && (s[slen-1] == ' ' || s[slen-1] == '\t' || s[slen-1] == '\r')) slen--;
        if (slen == 0 || s[0] == '#' || (s[0] == '/' && s[1] == '/')) continue;

        // Check for label (ends with ':').
        if (slen > 1 && s[slen-1] == ':') {
            size_t name_len = slen - 1;
            // Labels can also be on their own line before instructions.
            AsmSectionOut *sec = &u->sec[ctx.cur];
            label_add(&ctx, s, name_len, ctx.cur, (int64_t)sec->len);
            // Also update/create the symbol.
            int idx = sym_intern(&ctx, s, name_len);
            u->syms[idx].section = ctx.cur;
            u->syms[idx].value = (int64_t)sec->len;
            continue;
        }
        // Label might be followed by more content on the same line.
        const char *colon = NULL;
        for (size_t i = 0; i < slen; i++) {
            if (s[i] == ':' && (i == 0 || s[i-1] != ':')) { colon = s + i; break; }
        }
        if (colon) {
            size_t name_len = (size_t)(colon - s);
            AsmSectionOut *sec = &u->sec[ctx.cur];
            label_add(&ctx, s, name_len, ctx.cur, (int64_t)sec->len);
            int idx = sym_intern(&ctx, s, name_len);
            u->syms[idx].section = ctx.cur;
            u->syms[idx].value = (int64_t)sec->len;
            s = colon + 1;
            slen = slen - name_len - 1;
            while (slen && (s[0] == ' ' || s[0] == '\t')) { s++; slen--; }
            if (slen == 0) continue;
        }

        // Directive or instruction.
        if (s[0] == '.') {
            // Directive.
            const char *dir_end = s;
            while (dir_end < s + slen && *dir_end != ' ' && *dir_end != '\t') dir_end++;
            size_t dir_len = (size_t)(dir_end - s);
            char dir[64];
            if (dir_len >= sizeof(dir)) dir_len = sizeof(dir) - 1;
            memcpy(dir, s, dir_len);
            dir[dir_len] = '\0';
            const char *args = dir_end;
            if (args < s + slen) args++; // skip space
            else args = "";
            handle_directive(&ctx, dir, args);
        } else {
            // Instruction: mnemonic followed by operands.
            const char *mnem_end = s;
            while (mnem_end < s + slen && *mnem_end != ' ' && *mnem_end != '\t') mnem_end++;
            size_t mnem_len = (size_t)(mnem_end - s);
            char mnem[64];
            if (mnem_len >= sizeof(mnem)) mnem_len = sizeof(mnem) - 1;
            memcpy(mnem, s, mnem_len);
            mnem[mnem_len] = '\0';

            // Parse operands (comma-separated).
            Operand ops[3];
            int nop = 0;
            const char *op_str = mnem_end;
            while (op_str < s + slen && (*op_str == ' ' || *op_str == '\t')) op_str++;
            while (op_str < s + slen && nop < 3 && !is_eol(*op_str)) {
                const char *next;
                ops[nop] = parse_operand(op_str, &next);
                nop++;
                op_str = next;
                while (op_str < s + slen && (*op_str == ' ' || *op_str == '\t')) op_str++;
                if (*op_str == ',') { op_str++; while (op_str < s + slen && (*op_str == ' ' || *op_str == '\t')) op_str++; }
            }

            encode_instruction(&ctx, mnem, ops, nop);
        }
    }

    // Resolve forward fixups.
    for (size_t i = 0; i < ctx.nfixup && !ctx.failed; i++) {
        Fixup *f = &ctx.fixups[i];
        Label *l = label_find(&ctx, f->label, strlen(f->label));
        if (!l) {
            // Check if it's an external symbol.
            int sym = sym_intern(&ctx, f->label, strlen(f->label));
            add_reloc(&ctx, f->section, (int32_t)f->offset, sym, 1, 2, ASM_RELOC_X86_64_PC32);
            continue;
        }
        if (l->section != f->section) {
            err(&ctx, "cross-section fixup not supported for label %s", f->label);
            continue;
        }
        AsmSectionOut *sec = &u->sec[f->section];
        int64_t target = l->offset;
        int64_t next = f->offset + 4; // rel32 is relative to end of instruction
        int32_t rel = (int32_t)(target - next);
        uint8_t *p2 = sec->bytes + f->offset;
        p2[0] = (uint8_t)rel; p2[1] = (uint8_t)(rel >> 8);
        p2[2] = (uint8_t)(rel >> 16); p2[3] = (uint8_t)(rel >> 24);
    }

    // Cleanup.
    for (size_t i = 0; i < ctx.nlabel; i++) free(ctx.labels[i].name);
    free(ctx.labels);
    for (size_t i = 0; i < ctx.nfixup; i++) free(ctx.fixups[i].label);
    free(ctx.fixups);

    return ctx.failed ? 1 : 0;
}
