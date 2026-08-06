// M17.4.1: integrated riscv64 assembler for the QBE assembly subset.
//
// QBE (third_party/qbe, rv64 target, Gaself flavor) emits a small,
// stable subset of gas-style RISC-V assembly (see rv64/emit.c, gas.c).
// This module parses exactly that subset in a single pass and produces
// section contents, an internal symbol table, and relocations in memory.
// RISC-V pseudo-instructions that GNU as expands into instruction
// sequences (`la`, `call`, symbol-relative loads/stores, `li`, `mv`,
// `sext.*`/`zext.*`, `neg`, `seqz`, `snez`, `beqz`/`bnez`, `ret`) are
// expanded here with explicit paired PC-relative relocations
// (R_RISCV_PCREL_HI20 + R_RISCV_PCREL_LO12_*); the auipc always
// immediately precedes its lo12 instruction, so the linker locates each
// HI20 by adjacency. Anything outside the subset fails closed with a
// located diagnostic; partial objects are never produced.

#include "../include/asm_rv64.h"
#include "../include/elf_link.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- register names (RISC-V ABI numbering) --------------------------------

struct RvReg { const char *name; int nr; };

static const struct RvReg GRegs[] = {
    { "zero", 0 }, { "ra", 1 }, { "sp", 2 }, { "gp", 3 }, { "tp", 4 },
    { "t0", 5 }, { "t1", 6 }, { "t2", 7 }, { "s0", 8 }, { "fp", 8 },
    { "s1", 9 }, { "a0", 10 }, { "a1", 11 }, { "a2", 12 }, { "a3", 13 },
    { "a4", 14 }, { "a5", 15 }, { "a6", 16 }, { "a7", 17 }, { "s2", 18 },
    { "s3", 19 }, { "s4", 20 }, { "s5", 21 }, { "s6", 22 }, { "s7", 23 },
    { "s8", 24 }, { "s9", 25 }, { "s10", 26 }, { "s11", 27 }, { "t3", 28 },
    { "t4", 29 }, { "t5", 30 }, { "t6", 31 },
};

static const struct RvReg FRegs[] = {
    { "ft0", 0 }, { "ft1", 1 }, { "ft2", 2 }, { "ft3", 3 }, { "ft4", 4 },
    { "ft5", 5 }, { "ft6", 6 }, { "ft7", 7 }, { "fs0", 8 }, { "fs1", 9 },
    { "fa0", 10 }, { "fa1", 11 }, { "fa2", 12 }, { "fa3", 13 }, { "fa4", 14 },
    { "fa5", 15 }, { "fa6", 16 }, { "fa7", 17 }, { "fs2", 18 }, { "fs3", 19 },
    { "fs4", 20 }, { "fs5", 21 }, { "fs6", 22 }, { "fs7", 23 }, { "fs8", 24 },
    { "fs9", 25 }, { "fs10", 26 }, { "fs11", 27 }, { "ft8", 28 }, { "ft9", 29 },
    { "ft10", 30 }, { "ft11", 31 },
};

#define GRA 1
#define GX0 0

// --- internal structures ----------------------------------------------------

typedef struct {
    char *name;      // owned
    int section;     // ASM_SEC_* index
    int64_t offset;
} Label;

typedef struct {
    int64_t offset;  // offset of the branch/jump instruction in .text
    int line;
    int kind;        // 0 = B-type branch (beqz/bnez), 1 = J-type jump (j)
    int bne;         // B-type: funct3 (1 = bne, 0 = beq)
    char *label;     // owned
} Fixup;

typedef struct {
    AsmUnit *u;
    int cur;         // current section, -1 before any section directive
    int line;
    Label *labels;
    size_t nlabel, label_cap;
    Fixup *fixups;
    size_t nfixup, fixup_cap;
    int failed;
} Ctx;

// --- error reporting -----------------------------------------------------------

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

// --- section helpers ------------------------------------------------------------

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

static void sec_put32(Ctx *c, uint32_t w) {
    uint8_t b[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
    sec_put(c, b, 4);
}

static void sec_align(Ctx *c, size_t align) {
    AsmSectionOut *s = &c->u->sec[c->cur];
    size_t rem = s->len % align;
    if (rem == 0) return;
    uint8_t zero = 0;
    for (size_t i = 0; i < align - rem; i++) sec_put(c, &zero, 1);
}

static void add_reloc(Ctx *c, int section, int32_t address, int sym,
                      unsigned length, unsigned type, int64_t addend,
                      int32_t pair) {
    AsmSectionOut *s = &c->u->sec[section];
    if (s->nreloc >= s->reloc_cap) {
        size_t cap = s->reloc_cap ? s->reloc_cap * 2 : 8;
        s->relocs = realloc(s->relocs, cap * sizeof(AsmReloc));
        s->reloc_cap = cap;
    }
    s->relocs[s->nreloc].address = address;
    s->relocs[s->nreloc].symbol = sym;
    s->relocs[s->nreloc].pcrel = 1;
    s->relocs[s->nreloc].length = length;
    s->relocs[s->nreloc].type = type;
    s->relocs[s->nreloc].addend = addend;
    s->relocs[s->nreloc].pair = pair;
    s->nreloc++;
}

// --- symbol / label tables --------------------------------------------------------

static int sym_find(AsmUnit *u, const char *name, size_t len) {
    for (size_t i = 0; i < u->nsym; i++)
        if (strlen(u->syms[i].name) == len && memcmp(u->syms[i].name, name, len) == 0)
            return (int)i;
    return -1;
}

// Find or intern a symbol (undefined until a label defines it).
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

static void fixup_add(Ctx *c, int64_t offset, int kind, int bne,
                      const char *label, size_t len) {
    if (c->nfixup >= c->fixup_cap) {
        size_t cap = c->fixup_cap ? c->fixup_cap * 2 : 32;
        c->fixups = realloc(c->fixups, cap * sizeof(Fixup));
        c->fixup_cap = cap;
    }
    char *copy = malloc(len + 1);
    memcpy(copy, label, len);
    copy[len] = '\0';
    c->fixups[c->nfixup].offset = offset;
    c->fixups[c->nfixup].line = c->line;
    c->fixups[c->nfixup].kind = kind;
    c->fixups[c->nfixup].bne = bne;
    c->fixups[c->nfixup].label = copy;
    c->nfixup++;
}

// --- token helpers ---------------------------------------------------------------

static const char *trim(const char *s, size_t *len) {
    size_t n = *len;
    while (n && (s[0] == ' ' || s[0] == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    *len = n;
    return s;
}

static int parse_gpr(const char *s, size_t len) {
    for (size_t i = 0; i < sizeof(GRegs) / sizeof(GRegs[0]); i++)
        if (strlen(GRegs[i].name) == len && memcmp(GRegs[i].name, s, len) == 0)
            return GRegs[i].nr;
    return -1;
}

static int parse_fpr(const char *s, size_t len) {
    for (size_t i = 0; i < sizeof(FRegs) / sizeof(FRegs[0]); i++)
        if (strlen(FRegs[i].name) == len && memcmp(FRegs[i].name, s, len) == 0)
            return FRegs[i].nr;
    return -1;
}

// Parse a possibly-negative decimal/hex integer up to the full 64-bit
// unsigned range; returned as two's-complement int64.
static int parse_u64(const char *s, size_t len, int64_t *out) {
    if (len == 0) return -1;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    else if (s[0] == '+') i = 1;
    if (i >= len) return -1;
    int base = 10;
    if (len - i >= 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    if (i >= len) return -1;
    uint64_t v = 0;
    for (; i < len; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return -1;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return -1;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = neg ? (int64_t)(0ULL - v) : (int64_t)v;
    return 0;
}

// A memory/address operand: either `off(base)` or a symbol reference
// (`sym`, `sym+off`, `sym-off`, optionally followed by a GPR scratch
// register that materializes the address).
typedef struct {
    int is_sym;        // 1 = symbol, 0 = base+offset
    int64_t off;       // base+offset form: signed offset
    int base;          // base+offset form: GPR index
    const char *sym;   // symbol form: name
    size_t symlen;
    int64_t addend;    // symbol form: +/- addend
    int scratch;       // symbol form: GPR holding the address, -1 if none
} MemOp;

static int parse_mem(const char *s, size_t len, MemOp *m) {
    memset(m, 0, sizeof(*m));
    m->scratch = -1;
    size_t comma = len;
    for (size_t i = 0; i < len; i++)
        if (s[i] == ',') { comma = i; break; }
    const char *a = s;
    size_t alen = (comma < len) ? comma : len;
    a = trim(a, &alen);
    if (alen == 0) return -1;

    size_t paren = alen;
    for (size_t i = 0; i < alen; i++)
        if (a[i] == '(') { paren = i; break; }
    if (paren < alen) {
        if (a[alen - 1] != ')') return -1;
        if (comma < len) return -1; // scratch only valid with a symbol
        size_t offlen = paren;
        const char *os = trim(a, &offlen);
        m->off = 0;
        if (offlen > 0 && parse_u64(os, offlen, &m->off) != 0) return -1;
        const char *rs = a + paren + 1;
        size_t rlen = alen - paren - 2;
        rs = trim(rs, &rlen);
        int reg = parse_gpr(rs, rlen);
        if (reg < 0) return -1;
        m->base = reg;
        return 0;
    }

    m->is_sym = 1;
    size_t nlen = alen;
    const char *nm = a;
    int64_t addend = 0;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] == '+' || a[i] == '-') {
            if (parse_u64(a + i, alen - i, &addend) != 0) return -1;
            nlen = i;
            break;
        }
    }
    m->sym = nm;
    m->symlen = nlen;
    m->addend = addend;
    if (comma < len) {
        const char *rs = s + comma + 1;
        size_t rlen = len - comma - 1;
        rs = trim(rs, &rlen);
        int reg = parse_gpr(rs, rlen);
        if (reg < 0) return -1;
        m->scratch = reg;
    }
    return 0;
}

// Split an operand list at top-level commas.
static int split_ops(const char *s, size_t len, const char **ops, size_t *lens) {
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        char ch = (i < len) ? s[i] : ',';
        if (ch == ',') {
            if (count >= 6) return -1;
            size_t l = i - start;
            const char *p = trim(s + start, &l);
            ops[count] = p;
            lens[count] = l;
            count++;
            start = i + 1;
        }
    }
    return count;
}

// --- instruction encoders ------------------------------------------------------------

static uint32_t enc_r(uint32_t funct7, uint32_t rs2, uint32_t rs1,
                      uint32_t funct3, uint32_t rd, uint32_t opcode) {
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | opcode;
}

static uint32_t enc_i(uint32_t imm, uint32_t rs1, uint32_t funct3,
                      uint32_t rd, uint32_t opcode) {
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (funct3 << 12) |
           (rd << 7) | opcode;
}

static uint32_t enc_s(uint32_t imm, uint32_t rs2, uint32_t rs1,
                      uint32_t funct3, uint32_t opcode) {
    uint32_t i = imm & 0xFFF;
    return ((i >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | ((i & 0x1F) << 7) | opcode;
}

static uint32_t enc_b(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t funct3) {
    uint32_t i = imm & 0x1FFF;
    return ((i >> 12) << 31) | (((i >> 5) & 0x3F) << 25) | (rs2 << 20) |
           (rs1 << 15) | (funct3 << 12) | (((i >> 1) & 0xF) << 8) |
           (((i >> 11) & 1) << 7) | 0x63;
}

static uint32_t enc_j(uint32_t imm, uint32_t rd) {
    uint32_t i = imm & 0x1FFFFF;
    return ((i >> 20) << 31) | (((i >> 1) & 0x3FF) << 21) | (((i >> 11) & 1) << 20) |
           (((i >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F;
}

// --- instruction emitters -----------------------------------------------------------

static void emit_insn(Ctx *c, uint32_t word) { sec_put32(c, word); }

// li rd, n (gas semantics, recursively): the minimal instruction sequence
// materializing an arbitrary signed 64-bit constant. Matches the
// riscv-gnu-toolchain emit_li algorithm (verified against 200k values).
static void emit_li(Ctx *c, int rd, int64_t n) {
    if (n >= -2048 && n <= 2047) {
        emit_insn(c, enc_i((uint32_t)n, GX0, 0, (uint32_t)rd, 0x13)); // addi
        return;
    }
    int64_t hi = (n + 0x800) >> 12;
    int64_t lo = n - (hi << 12);
    if (hi >= -0x80000 && hi <= 0x7FFFF) {
        // lui rd, hi (20-bit signed, sign-extended)
        emit_insn(c, 0x37u | (uint32_t)(rd << 7) | ((uint32_t)(hi & 0xFFFFF) << 12));
        if (lo != 0)
            emit_insn(c, enc_i((uint32_t)lo, (uint32_t)rd, 0, (uint32_t)rd, 0x13)); // addi
        return;
    }
    emit_li(c, rd, hi);
    emit_insn(c, enc_i(12, (uint32_t)rd, 1, (uint32_t)rd, 0x13)); // slli rd, rd, 12
    emit_insn(c, enc_i((uint32_t)lo, (uint32_t)rd, 0, (uint32_t)rd, 0x13)); // addi
}

// Emit auipc rd, hi20(sym); the paired lo12 instruction follows.
static void emit_auipc(Ctx *c, int rd, int sym, int64_t addend) {
    size_t at = c->u->sec[ASM_SEC_TEXT].len;
    emit_insn(c, 0x17u | (uint32_t)(rd << 7)); // auipc rd, 0
    add_reloc(c, ASM_SEC_TEXT, (int32_t)at, sym, 2, ASM_RELOC_RISCV_HI20, addend, -1);
}

static void emit_lopc_i(Ctx *c, uint32_t funct3, int rd, int rs1, uint32_t opcode,
                        int sym, int64_t addend, int64_t auipc_off) {
    emit_insn(c, enc_i((uint32_t)addend, (uint32_t)rs1, funct3, (uint32_t)rd, opcode));
    if (sym >= 0)
        add_reloc(c, ASM_SEC_TEXT, (int32_t)(c->u->sec[ASM_SEC_TEXT].len - 4), sym,
                  0, ASM_RELOC_RISCV_LO12_I, addend, (int32_t)auipc_off);
}

static void emit_lopc_s(Ctx *c, int rs2, int rs1, uint32_t funct3, uint32_t opcode,
                        int sym, int64_t addend, int64_t auipc_off) {
    emit_insn(c, enc_s((uint32_t)addend, (uint32_t)rs2, (uint32_t)rs1, funct3, opcode));
    if (sym >= 0)
        add_reloc(c, ASM_SEC_TEXT, (int32_t)(c->u->sec[ASM_SEC_TEXT].len - 4), sym,
                  0, ASM_RELOC_RISCV_LO12_S, addend, (int32_t)auipc_off);
}

// --- instruction dispatch ----------------------------------------------------------

static int req_text(Ctx *c) {
    if (c->cur != ASM_SEC_TEXT) { err(c, "instruction outside .text section"); return -1; }
    return 0;
}

// Integer ALU/bitwise ops, both register and immediate (I-type) forms.
// Immediate-capable ops are those QBE marks V(1) in ops.h: add, and, or,
// xor, sll, srl, sra, slt, sltu.
typedef struct {
    const char *name;   // assembly mnemonic (with %k suffix resolved)
    int f7;             // funct7
    int f3;             // funct3
    int opc;            // opcode (0x33 = OP, 0x3B = OP-32)
    int has_imm;        // supports an immediate (or shift amount) operand
} AluOp;

static const AluOp alu_ops[] = {
    { "add", 0x00, 0, 0x33, 1 }, { "addw", 0x00, 0, 0x3B, 1 },
    { "sub", 0x20, 0, 0x33, 0 }, { "subw", 0x20, 0, 0x3B, 0 },
    { "and", 0x00, 7, 0x33, 1 },
    { "or",  0x00, 6, 0x33, 1 },
    { "xor", 0x00, 4, 0x33, 1 },
    { "sll", 0x00, 1, 0x33, 1 }, { "sllw", 0x00, 1, 0x3B, 1 },
    { "srl", 0x00, 5, 0x33, 1 }, { "srlw", 0x00, 5, 0x3B, 1 },
    { "sra", 0x20, 5, 0x33, 1 }, { "sraw", 0x20, 5, 0x3B, 1 },
    { "slt", 0x00, 2, 0x33, 1 },
    { "sltu", 0x00, 3, 0x33, 1 },
    { "mul", 0x01, 0, 0x33, 0 }, { "mulw", 0x01, 0, 0x3B, 0 },
    { "div", 0x01, 4, 0x33, 0 }, { "divw", 0x01, 4, 0x3B, 0 },
    { "divu", 0x01, 5, 0x33, 0 }, { "divuw", 0x01, 5, 0x3B, 0 },
    { "rem", 0x01, 6, 0x33, 0 }, { "remw", 0x01, 6, 0x3B, 0 },
    { "remu", 0x01, 7, 0x33, 0 }, { "remuw", 0x01, 7, 0x3B, 0 },
};

// Emit the immediate form: addi/addiw, andi/ori/xori, slti/sltiu, or
// shift-immediate slli/srli/srai (with shamt in imm[5:0]).
static int emit_alu_imm(Ctx *c, const AluOp *op, int rd, int rs1,
                        const char *immstr, size_t immlen) {
    int64_t v;
    if (parse_u64(immstr, immlen, &v) != 0) {
        err(c, "malformed immediate in '%s'", op->name);
        return -1;
    }
    int is_word = op->opc == 0x3B;
    const char *n = op->name;
    if (n[0] == 'a') { // add -> addi
        if (v < -2048 || v > 2047) { err(c, "immediate out of range in '%s'", n); return -1; }
        emit_insn(c, enc_i((uint32_t)v, (uint32_t)rs1, 0, (uint32_t)rd, is_word ? 0x1B : 0x13));
        return 0;
    }
    if (n[0] == 's' && (n[1] == 'l' || n[1] == 'r')) { // shift amount
        int64_t max = is_word ? 31 : 63;
        if (v < 0 || v > max) { err(c, "shift amount out of range in '%s'", n); return -1; }
        uint32_t imm12 = (uint32_t)v;
        uint32_t f3 = (uint32_t)op->f3;
        if (n[2] == 'a') imm12 |= 0x400; // srai/sraiw
        emit_insn(c, enc_i(imm12, (uint32_t)rs1, f3, (uint32_t)rd, is_word ? 0x1B : 0x13));
        return 0;
    }
    if (n[0] == 's' && n[1] == 'l' && n[2] == 't') { // slt/sltu -> slti/sltiu
        if (v < -2048 || v > 2047) { err(c, "immediate out of range in '%s'", n); return -1; }
        emit_insn(c, enc_i((uint32_t)v, (uint32_t)rs1, (uint32_t)op->f3, (uint32_t)rd, 0x13));
        return 0;
    }
    // and/or/xor -> andi/ori/xori
    if (v < -2048 || v > 2047) { err(c, "immediate out of range in '%s'", n); return -1; }
    emit_insn(c, enc_i((uint32_t)v, (uint32_t)rs1, (uint32_t)op->f3, (uint32_t)rd, 0x13));
    return 0;
}

static int assemble_alu(Ctx *c, const char *mn, size_t mnlen,
                        const char **ops, size_t *lens, int nops) {
    const AluOp *op = NULL;
    for (size_t i = 0; i < sizeof(alu_ops) / sizeof(alu_ops[0]); i++)
        if (strlen(alu_ops[i].name) == mnlen && memcmp(alu_ops[i].name, mn, mnlen) == 0) {
            op = &alu_ops[i];
            break;
        }
    if (!op) return 1; // not an ALU op
    if (nops != 3) { err(c, "expected 3 operands for '%s'", op->name); return -1; }
    int rd = parse_gpr(ops[0], lens[0]);
    int rs1 = parse_gpr(ops[1], lens[1]);
    if (rd < 0 || rs1 < 0) { err(c, "bad register operand in '%s'", op->name); return -1; }
    // Immediate second operand (QBE emits one when the value is in range).
    if (lens[2] > 0 && (ops[2][0] == '-' || ops[2][0] == '+' || ops[2][0] == '0')) {
        int is_num = 1;
        for (size_t i = 1; i < lens[2]; i++)
            if (!(ops[2][i] >= '0' && ops[2][i] <= '9') && !(ops[2][i] >= 'a' && ops[2][i] <= 'f') &&
                !(ops[2][i] >= 'A' && ops[2][i] <= 'F') && ops[2][i] != 'x' && ops[2][i] != 'X') {
                is_num = 0; break;
            }
        if (is_num && op->has_imm)
            return emit_alu_imm(c, op, rd, rs1, ops[2], lens[2]);
    }
    int rs2 = parse_gpr(ops[2], lens[2]);
    if (rs2 < 0) { err(c, "bad register operand in '%s'", op->name); return -1; }
    emit_insn(c, enc_r((uint32_t)op->f7, (uint32_t)rs2, (uint32_t)rs1,
                       (uint32_t)op->f3, (uint32_t)rd, (uint32_t)op->opc));
    return 0;
}

// Loads: name -> (funct3, opcode, float?)
static const struct { const char *n; int f3; int opc; int fp; } loads[] = {
    { "lb", 0, 0x03, 0 }, { "lbu", 4, 0x03, 0 },
    { "lh", 1, 0x03, 0 }, { "lhu", 5, 0x03, 0 },
    { "lw", 2, 0x03, 0 }, { "lwu", 6, 0x03, 0 },
    { "ld", 3, 0x03, 0 },
    { "flw", 2, 0x07, 1 }, { "fld", 3, 0x07, 1 },
};

// Stores: name -> (funct3, opcode, float?)
static const struct { const char *n; int f3; int opc; int fp; } stores[] = {
    { "sb", 0, 0x23, 0 }, { "sh", 1, 0x23, 0 },
    { "sw", 2, 0x23, 0 }, { "sd", 3, 0x23, 0 },
    { "fsw", 2, 0x27, 1 }, { "fsd", 3, 0x27, 1 },
};

static int assemble_load(Ctx *c, const char *mn, int f3, int opc, int fp,
                         const char *rest, size_t rest_len) {
    size_t comma = rest_len;
    for (size_t i = 0; i < rest_len; i++)
        if (rest[i] == ',') { comma = i; break; }
    if (comma >= rest_len) { err(c, "expected a memory operand in '%s'", mn); return -1; }
    const char *rdstr = rest;
    size_t rdlen = comma;
    rdstr = trim(rdstr, &rdlen);
    const char *memstr = rest + comma + 1;
    size_t memlen = rest_len - comma - 1;
    memstr = trim(memstr, &memlen);
    int rd = fp ? parse_fpr(rdstr, rdlen) : parse_gpr(rdstr, rdlen);
    if (rd < 0) { err(c, "bad destination register in '%s'", mn); return -1; }
    MemOp m;
    if (parse_mem(memstr, memlen, &m) != 0) { err(c, "malformed memory operand in '%s'", mn); return -1; }
    if (!m.is_sym) {
        if (m.off < -2048 || m.off > 2047) { err(c, "load offset out of range in '%s'", mn); return -1; }
        emit_insn(c, enc_i((uint32_t)m.off, (uint32_t)m.base, (uint32_t)f3,
                           (uint32_t)rd, (uint32_t)opc));
        return 0;
    }
    int sym = sym_intern(c, m.sym, m.symlen);
    int scratch = m.scratch;
    if (fp && scratch < 0) {
        err(c, "float load '%s' from a symbol requires a scratch register", mn);
        return -1;
    }
    if (scratch < 0) scratch = rd; // integer load reuses the destination
    int64_t auipc_off = (int64_t)c->u->sec[ASM_SEC_TEXT].len;
    emit_auipc(c, scratch, sym, m.addend);
    emit_lopc_i(c, (uint32_t)f3, rd, scratch, (uint32_t)opc, sym, m.addend, auipc_off);
    return 0;
}

static int assemble_store(Ctx *c, const char *mn, int f3, int opc, int fp,
                          const char *rest, size_t rest_len) {
    size_t comma = rest_len;
    for (size_t i = 0; i < rest_len; i++)
        if (rest[i] == ',') { comma = i; break; }
    if (comma >= rest_len) { err(c, "expected a memory operand in '%s'", mn); return -1; }
    const char *valstr = rest;
    size_t vallen = comma;
    valstr = trim(valstr, &vallen);
    const char *memstr = rest + comma + 1;
    size_t memlen = rest_len - comma - 1;
    memstr = trim(memstr, &memlen);
    int val = fp ? parse_fpr(valstr, vallen) : parse_gpr(valstr, vallen);
    if (val < 0) { err(c, "bad source register in '%s'", mn); return -1; }
    MemOp m;
    if (parse_mem(memstr, memlen, &m) != 0) { err(c, "malformed memory operand in '%s'", mn); return -1; }
    if (!m.is_sym) {
        if (m.off < -2048 || m.off > 2047) { err(c, "store offset out of range in '%s'", mn); return -1; }
        emit_insn(c, enc_s((uint32_t)m.off, (uint32_t)val, (uint32_t)m.base,
                           (uint32_t)f3, (uint32_t)opc));
        return 0;
    }
    if (m.scratch < 0) { err(c, "store '%s' to a symbol requires a scratch register", mn); return -1; }
    if (m.scratch == val) { err(c, "store value register must not be the address scratch in '%s'", mn); return -1; }
    int sym = sym_intern(c, m.sym, m.symlen);
    int64_t auipc_off = (int64_t)c->u->sec[ASM_SEC_TEXT].len;
    emit_auipc(c, m.scratch, sym, m.addend);
    emit_lopc_s(c, val, m.scratch, (uint32_t)f3, (uint32_t)opc, sym, m.addend, auipc_off);
    return 0;
}

// FP arithmetic: name -> (funct7, opcode 0x53, rm in funct3 = RNE).
static const struct { const char *n; int f7; int f3; } fparith[] = {
    { "fadd.d", 0x01, 0 }, { "fsub.d", 0x05, 0 },
    { "fmul.d", 0x09, 0 }, { "fdiv.d", 0x0D, 0 },
    { "fadd.s", 0x00, 0 }, { "fsub.s", 0x04, 0 },
    { "fmul.s", 0x08, 0 }, { "fdiv.s", 0x0C, 0 },
};

// FP unary: fneg (fsgnjn), fabs (fsgnjx), fsqrt.
static const struct { const char *n; int f7; int f3; int fsqrt; } fpuni[] = {
    { "fneg.d", 0x21, 1, 0 }, { "fneg.s", 0x20, 1, 0 },
    { "fabs.d", 0x21, 2, 0 }, { "fabs.s", 0x20, 2, 0 },
    { "fsqrt.d", 0x2D, 0, 1 }, { "fsqrt.s", 0x2C, 0, 1 },
};

// FP compare producing a GPR: feq/flt/fle + pseudo fgt/fge (operand swap).
static const struct { const char *n; int f7; int f3; int swap; } fpcmp[] = {
    { "feq.d", 0x51, 2, 0 }, { "flt.d", 0x51, 1, 0 }, { "fle.d", 0x51, 0, 0 },
    { "fgt.d", 0x51, 1, 1 }, { "fge.d", 0x51, 0, 1 },
    { "feq.s", 0x50, 2, 0 }, { "flt.s", 0x50, 1, 0 }, { "fle.s", 0x50, 0, 0 },
    { "fgt.s", 0x50, 1, 1 }, { "fge.s", 0x50, 0, 1 },
};

// fcvt: name -> (funct7, rs2-format field, dest float?).
static const struct { const char *n; int f7; int rs2; int dstfp; } fcvts[] = {
    { "fcvt.w.d", 0x61, 0x00, 0 }, { "fcvt.wu.d", 0x61, 0x01, 0 },
    { "fcvt.l.d", 0x61, 0x02, 0 }, { "fcvt.lu.d", 0x61, 0x03, 0 },
    { "fcvt.w.s", 0x60, 0x00, 0 }, { "fcvt.wu.s", 0x60, 0x01, 0 },
    { "fcvt.l.s", 0x60, 0x02, 0 }, { "fcvt.lu.s", 0x60, 0x03, 0 },
    { "fcvt.d.w", 0x69, 0x00, 1 }, { "fcvt.d.wu", 0x69, 0x01, 1 },
    { "fcvt.d.l", 0x69, 0x02, 1 }, { "fcvt.d.lu", 0x69, 0x03, 1 },
    { "fcvt.s.w", 0x68, 0x00, 1 }, { "fcvt.s.wu", 0x68, 0x01, 1 },
    { "fcvt.s.l", 0x68, 0x02, 1 }, { "fcvt.s.lu", 0x68, 0x03, 1 },
    { "fcvt.s.d", 0x40, 0x01, 1 }, { "fcvt.d.s", 0x41, 0x00, 1 },
};

// fmv moves: name -> (funct7, dest float?).
static const struct { const char *n; int f7; int dstfp; } fmvs[] = {
    { "fmv.x.w", 0x70, 0 }, { "fmv.x.d", 0x71, 0 },
    { "fmv.w.x", 0x78, 1 }, { "fmv.d.x", 0x79, 1 },
};

static int assemble_fparith(Ctx *c, const char **ops, size_t *lens, int nops,
                            int f7) {
    if (nops != 3) { err(c, "expected 3 operands"); return -1; }
    int rd = parse_fpr(ops[0], lens[0]);
    int rs1 = parse_fpr(ops[1], lens[1]);
    int rs2 = parse_fpr(ops[2], lens[2]);
    if (rd < 0 || rs1 < 0 || rs2 < 0) { err(c, "bad register operand"); return -1; }
    emit_insn(c, enc_r((uint32_t)f7, (uint32_t)rs2, (uint32_t)rs1,
                       0 /* RNE */, (uint32_t)rd, 0x53));
    return 0;
}

static int assemble_fpuni(Ctx *c, const char **ops, size_t *lens, int nops,
                          int f7, int f3, int fsqrt) {
    if (nops != 2) { err(c, "expected 2 operands"); return -1; }
    int rd = parse_fpr(ops[0], lens[0]);
    int rs = parse_fpr(ops[1], lens[1]);
    if (rd < 0 || rs < 0) { err(c, "bad register operand"); return -1; }
    uint32_t rs2 = fsqrt ? 0u : (uint32_t)rs;
    uint32_t rs1 = fsqrt ? (uint32_t)rs : (uint32_t)rs;
    emit_insn(c, enc_r((uint32_t)f7, rs2, rs1, (uint32_t)f3, (uint32_t)rd, 0x53));
    return 0;
}

static int assemble_fpcmp(Ctx *c, const char **ops, size_t *lens, int nops,
                          int f7, int f3, int swap) {
    if (nops != 3) { err(c, "expected 3 operands"); return -1; }
    int rd = parse_gpr(ops[0], lens[0]);
    int ra = parse_fpr(ops[1], lens[1]);
    int rb = parse_fpr(ops[2], lens[2]);
    if (rd < 0 || ra < 0 || rb < 0) { err(c, "bad register operand"); return -1; }
    uint32_t rs1 = swap ? (uint32_t)rb : (uint32_t)ra;
    uint32_t rs2 = swap ? (uint32_t)ra : (uint32_t)rb;
    emit_insn(c, enc_r((uint32_t)f7, rs2, rs1, (uint32_t)f3, (uint32_t)rd, 0x53));
    return 0;
}

static int assemble_fcvt(Ctx *c, const char *mn, int f7, int rs2code, int dstfp,
                         const char **ops, size_t *lens, int nops) {
    if (nops != 2 && nops != 3) { err(c, "expected 2 operands for '%s'", mn); return -1; }
    if (nops == 3) {
        // optional ", rtz" rounding-mode suffix (float -> int conversions)
        const char *rm = ops[2];
        size_t rml = lens[2];
        rm = trim(rm, &rml);
        if (rml != 3 || memcmp(rm, "rtz", 3) != 0) {
            err(c, "unsupported rounding mode in '%s'", mn);
            return -1;
        }
    }
    int rd, rs;
    if (dstfp) {
        rd = parse_fpr(ops[0], lens[0]);
        rs = parse_gpr(ops[1], lens[1]);
    } else {
        rd = parse_gpr(ops[0], lens[0]);
        rs = parse_fpr(ops[1], lens[1]);
    }
    if (rd < 0 || rs < 0) { err(c, "bad register operand in '%s'", mn); return -1; }
    uint32_t rm = (nops == 3) ? 1u /* RZ */ : 0u /* RNE */;
    emit_insn(c, enc_r((uint32_t)f7, (uint32_t)rs2code, (uint32_t)rs, rm,
                       (uint32_t)rd, 0x53));
    return 0;
}

static int assemble_fmv(Ctx *c, const char *mn, int f7, int dstfp,
                        const char **ops, size_t *lens, int nops) {
    if (nops != 2) { err(c, "expected 2 operands for '%s'", mn); return -1; }
    int rd, rs;
    if (dstfp) {
        rd = parse_fpr(ops[0], lens[0]);
        rs = parse_gpr(ops[1], lens[1]);
    } else {
        rd = parse_gpr(ops[0], lens[0]);
        rs = parse_fpr(ops[1], lens[1]);
    }
    if (rd < 0 || rs < 0) { err(c, "bad register operand in '%s'", mn); return -1; }
    emit_insn(c, enc_r((uint32_t)f7, 0, (uint32_t)rs, 0, (uint32_t)rd, 0x53));
    return 0;
}

static int assemble_insn(Ctx *c, const char *mn, size_t mnlen,
                         const char *rest, size_t rest_len) {
    if (req_text(c) != 0) return -1;
    const char *ops[6];
    size_t lens[6];
    int nops = split_ops(rest, rest_len, ops, lens);
    if (nops < 0) { err(c, "too many operands"); return -1; }

    // -- pseudo-instructions expanded by the assembler --
    if (mnlen == 2 && memcmp(mn, "li", 2) == 0) {
        if (nops != 2) { err(c, "li expects 2 operands"); return -1; }
        int rd = parse_gpr(ops[0], lens[0]);
        int64_t v;
        if (rd < 0 || parse_u64(ops[1], lens[1], &v) != 0) {
            err(c, "malformed 'li'"); return -1;
        }
        emit_li(c, rd, v);
        return 0;
    }
    if (mnlen == 2 && memcmp(mn, "la", 2) == 0) {
        if (nops != 2) { err(c, "la expects 2 operands"); return -1; }
        int rd = parse_gpr(ops[0], lens[0]);
        if (rd < 0) { err(c, "bad register in 'la'"); return -1; }
        MemOp m;
        if (parse_mem(ops[1], lens[1], &m) != 0 || !m.is_sym) {
            err(c, "la expects a symbol operand"); return -1;
        }
        int sym = sym_intern(c, m.sym, m.symlen);
        int64_t auipc_off = (int64_t)c->u->sec[ASM_SEC_TEXT].len;
        emit_auipc(c, rd, sym, m.addend);
        emit_lopc_i(c, 0, rd, rd, 0x13, sym, m.addend, auipc_off); // addi
        return 0;
    }
    if (mnlen == 2 && memcmp(mn, "mv", 2) == 0) {
        if (nops != 2) { err(c, "mv expects 2 operands"); return -1; }
        int rd = parse_gpr(ops[0], lens[0]);
        int rs = parse_gpr(ops[1], lens[1]);
        if (rd < 0 || rs < 0) { err(c, "bad register in 'mv'"); return -1; }
        emit_insn(c, enc_i(0, (uint32_t)rs, 0, (uint32_t)rd, 0x13)); // addi rd, rs, 0
        return 0;
    }
    if ((mnlen == 3 && memcmp(mn, "neg", 3) == 0) ||
        (mnlen == 4 && memcmp(mn, "negw", 4) == 0)) {
        if (nops != 2) { err(c, "neg expects 2 operands"); return -1; }
        int rd = parse_gpr(ops[0], lens[0]);
        int rs = parse_gpr(ops[1], lens[1]);
        if (rd < 0 || rs < 0) { err(c, "bad register in 'neg'"); return -1; }
        int word = mnlen == 4;
        emit_insn(c, enc_r(word ? 0x20 : 0x20, (uint32_t)rs, GX0, 0,
                           (uint32_t)rd, word ? 0x3B : 0x33)); // sub(w) rd, x0, rs
        return 0;
    }
    if (mnlen == 4 && (memcmp(mn, "seqz", 4) == 0 || memcmp(mn, "snez", 4) == 0)) {
        if (nops != 2) { err(c, "%s expects 2 operands", mn); return -1; }
        int rd = parse_gpr(ops[0], lens[0]);
        int rs = parse_gpr(ops[1], lens[1]);
        if (rd < 0 || rs < 0) { err(c, "bad register in '%.*s'", (int)mnlen, mn); return -1; }
        if (mn[1] == 'e') // seqz: sltiu rd, rs, 1
            emit_insn(c, enc_i(1, (uint32_t)rs, 3, (uint32_t)rd, 0x13));
        else              // snez: sltu rd, x0, rs
            emit_insn(c, enc_r(0, (uint32_t)rs, GX0, 3, (uint32_t)rd, 0x33));
        return 0;
    }
    if (mnlen >= 5 && (memcmp(mn, "sext.", 5) == 0 || memcmp(mn, "zext.", 5) == 0)) {
        if (nops != 2) { err(c, "%.*s expects 2 operands", (int)mnlen, mn); return -1; }
        int rd = parse_gpr(ops[0], lens[0]);
        int rs = parse_gpr(ops[1], lens[1]);
        if (rd < 0 || rs < 0) { err(c, "bad register in '%.*s'", (int)mnlen, mn); return -1; }
        char kind = mn[0];
        char size = mn[5];
        if (kind == 's' && size == 'b') { // slli 56 / srai 56
            emit_insn(c, enc_i(56, (uint32_t)rs, 1, (uint32_t)rd, 0x13));
            emit_insn(c, enc_i(56 | 0x400, (uint32_t)rd, 5, (uint32_t)rd, 0x13));
        } else if (kind == 's' && size == 'h') { // slli 48 / srai 48
            emit_insn(c, enc_i(48, (uint32_t)rs, 1, (uint32_t)rd, 0x13));
            emit_insn(c, enc_i(48 | 0x400, (uint32_t)rd, 5, (uint32_t)rd, 0x13));
        } else if (kind == 's' && size == 'w') { // addiw rd, rs, 0
            emit_insn(c, enc_i(0, (uint32_t)rs, 0, (uint32_t)rd, 0x1B));
        } else if (kind == 'z' && size == 'b') { // andi rd, rs, 255
            emit_insn(c, enc_i(255, (uint32_t)rs, 7, (uint32_t)rd, 0x13));
        } else if (kind == 'z' && size == 'h') { // slli 48 / srli 48
            emit_insn(c, enc_i(48, (uint32_t)rs, 1, (uint32_t)rd, 0x13));
            emit_insn(c, enc_i(48, (uint32_t)rd, 5, (uint32_t)rd, 0x13));
        } else if (kind == 'z' && size == 'w') { // slli 32 / srli 32
            emit_insn(c, enc_i(32, (uint32_t)rs, 1, (uint32_t)rd, 0x13));
            emit_insn(c, enc_i(32, (uint32_t)rd, 5, (uint32_t)rd, 0x13));
        } else {
            err(c, "unsupported extension '%.*s'", (int)mnlen, mn);
            return -1;
        }
        return 0;
    }

    // -- loads and stores --
    for (size_t i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
        if (strlen(loads[i].n) == mnlen && memcmp(loads[i].n, mn, mnlen) == 0)
            return assemble_load(c, loads[i].n, loads[i].f3, loads[i].opc,
                                 loads[i].fp, rest, rest_len);
    }
    for (size_t i = 0; i < sizeof(stores) / sizeof(stores[0]); i++) {
        if (strlen(stores[i].n) == mnlen && memcmp(stores[i].n, mn, mnlen) == 0)
            return assemble_store(c, stores[i].n, stores[i].f3, stores[i].opc,
                                  stores[i].fp, rest, rest_len);
    }

    // -- FP arithmetic --
    for (size_t i = 0; i < sizeof(fparith) / sizeof(fparith[0]); i++) {
        if (strlen(fparith[i].n) == mnlen && memcmp(fparith[i].n, mn, mnlen) == 0)
            return assemble_fparith(c, ops, lens, nops, fparith[i].f7);
    }
    for (size_t i = 0; i < sizeof(fpuni) / sizeof(fpuni[0]); i++) {
        if (strlen(fpuni[i].n) == mnlen && memcmp(fpuni[i].n, mn, mnlen) == 0)
            return assemble_fpuni(c, ops, lens, nops, fpuni[i].f7, fpuni[i].f3,
                                  fpuni[i].fsqrt);
    }
    for (size_t i = 0; i < sizeof(fpcmp) / sizeof(fpcmp[0]); i++) {
        if (strlen(fpcmp[i].n) == mnlen && memcmp(fpcmp[i].n, mn, mnlen) == 0)
            return assemble_fpcmp(c, ops, lens, nops, fpcmp[i].f7, fpcmp[i].f3,
                                  fpcmp[i].swap);
    }
    for (size_t i = 0; i < sizeof(fcvts) / sizeof(fcvts[0]); i++) {
        if (strlen(fcvts[i].n) == mnlen && memcmp(fcvts[i].n, mn, mnlen) == 0)
            return assemble_fcvt(c, fcvts[i].n, fcvts[i].f7, fcvts[i].rs2,
                                 fcvts[i].dstfp, ops, lens, nops);
    }
    for (size_t i = 0; i < sizeof(fmvs) / sizeof(fmvs[0]); i++) {
        if (strlen(fmvs[i].n) == mnlen && memcmp(fmvs[i].n, mn, mnlen) == 0)
            return assemble_fmv(c, fmvs[i].n, fmvs[i].f7, fmvs[i].dstfp,
                                ops, lens, nops);
    }
    if (mnlen == 3 && memcmp(mn, "fmv", 3) == 0) { // fmv.d / fmv.s (copy)
        if (mnlen == 5 && (mn[4] == 'd' || mn[4] == 's')) {
            if (nops != 2) { err(c, "%s expects 2 operands", mn); return -1; }
            int rd = parse_fpr(ops[0], lens[0]);
            int rs = parse_fpr(ops[1], lens[1]);
            if (rd < 0 || rs < 0) { err(c, "bad register in '%.*s'", (int)mnlen, mn); return -1; }
            int d = mn[4] == 'd';
            emit_insn(c, enc_r(d ? 0x21 : 0x20, (uint32_t)rs, (uint32_t)rs, 0,
                               (uint32_t)rd, 0x53)); // fsgnj
            return 0;
        }
    }

    // -- control flow --
    if (mnlen == 3 && memcmp(mn, "ret", 3) == 0) {
        if (nops != 0) { err(c, "ret expects no operands"); return -1; }
        emit_insn(c, enc_i(0, GRA, 0, GX0, 0x67)); // jalr x0, 0(ra)
        return 0;
    }
    if (mnlen == 1 && memcmp(mn, "j", 1) == 0) {
        if (nops != 1) { err(c, "j expects 1 operand"); return -1; }
        size_t tl = lens[0];
        const char *t = trim(ops[0], &tl);
        if (tl == 0) { err(c, "j without target label"); return -1; }
        emit_insn(c, enc_j(0, GX0)); // jal x0
        fixup_add(c, (int64_t)c->u->sec[ASM_SEC_TEXT].len - 4, 1, 0, t, tl);
        return 0;
    }
    if (mnlen == 4 && (memcmp(mn, "beqz", 4) == 0 || memcmp(mn, "bnez", 4) == 0)) {
        if (nops != 2) { err(c, "%s expects 2 operands", mn); return -1; }
        int rs = parse_gpr(ops[0], lens[0]);
        if (rs < 0) { err(c, "bad register in '%.*s'", (int)mnlen, mn); return -1; }
        size_t tl = lens[1];
        const char *t = trim(ops[1], &tl);
        if (tl == 0) { err(c, "%.*s without target label", (int)mnlen, mn); return -1; }
        int bne = mn[1] == 'n';
        emit_insn(c, enc_b(0, GX0, (uint32_t)rs, (uint32_t)(bne ? 1 : 0)));
        fixup_add(c, (int64_t)c->u->sec[ASM_SEC_TEXT].len - 4, 0, bne, t, tl);
        return 0;
    }
    if (mnlen == 4 && memcmp(mn, "call", 4) == 0) {
        if (nops != 1) { err(c, "call expects 1 operand"); return -1; }
        size_t sl = lens[0];
        const char *nm = trim(ops[0], &sl);
        if (sl == 0) { err(c, "call without target"); return -1; }
        int sym = sym_intern(c, nm, sl);
        int64_t auipc_off = (int64_t)c->u->sec[ASM_SEC_TEXT].len;
        emit_auipc(c, GRA, sym, 0);                                  // auipc ra, hi20
        emit_lopc_i(c, 0, GRA, GRA, 0x67, sym, 0, auipc_off);        // jalr ra, lo12(ra)
        return 0;
    }
    if (mnlen == 4 && memcmp(mn, "jalr", 4) == 0) {
        if (nops == 1) {
            int rs = parse_gpr(ops[0], lens[0]);
            if (rs < 0) { err(c, "bad register in 'jalr'"); return -1; }
            emit_insn(c, enc_i(0, (uint32_t)rs, 0, GRA, 0x67)); // jalr ra, 0(rs)
            return 0;
        }
        if (nops == 2) {
            int rd = parse_gpr(ops[0], lens[0]);
            if (rd < 0) { err(c, "bad register in 'jalr'"); return -1; }
            MemOp m;
            if (parse_mem(ops[1], lens[1], &m) != 0 || m.is_sym) {
                err(c, "jalr expects a register-offset operand"); return -1;
            }
            if (m.off < -2048 || m.off > 2047) { err(c, "jalr offset out of range"); return -1; }
            emit_insn(c, enc_i((uint32_t)m.off, (uint32_t)m.base, 0, (uint32_t)rd, 0x67));
            return 0;
        }
        err(c, "jalr expects 1 or 2 operands");
        return -1;
    }

    // -- integer ALU/bitwise --
    {
        int r = assemble_alu(c, mn, mnlen, ops, lens, nops);
        if (r == 0) return 0;
        if (r < 0) return -1; // error already reported
        err(c, "unsupported instruction '%.*s'", (int)mnlen, mn);
        return -1;
    }
}

// --- data directives ------------------------------------------------------------

// Parse a "..." gas string with escapes (same set as the arm64 assembler:
// \n \t \r \\ \" octal).
static int parse_ascii(Ctx *c, const char *s, size_t len) {
    if (len < 2 || s[0] != '"' || s[len - 1] != '"') {
        err(c, "malformed .ascii string");
        return -1;
    }
    for (size_t i = 1; i + 1 < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\\') {
            i++;
            if (i + 1 >= len) { err(c, "truncated escape in .ascii string"); return -1; }
            unsigned char e = (unsigned char)s[i];
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && i + 2 < len && s[i + 1] >= '0' && s[i + 1] <= '7'; k++) {
                            i++;
                            v = v * 8 + (s[i] - '0');
                        }
                        ch = (unsigned char)v;
                    } else {
                        err(c, "unsupported escape in .ascii string");
                        return -1;
                    }
            }
        }
        sec_put(c, &ch, 1);
    }
    return 0;
}

static int assemble_data_values(Ctx *c, int size, const char *rest, size_t rest_len) {
    AsmUnit *u = c->u;
    const char *ops[8];
    size_t lens[8];
    int n = split_ops(rest, rest_len, ops, lens);
    if (n <= 0) { err(c, "missing data value"); return -1; }
    for (int i = 0; i < n; i++) {
        if (lens[i] == 0) { err(c, "missing data value"); return -1; }
        int is_num = ops[i][0] == '-' || ops[i][0] == '+' ||
                     (ops[i][0] >= '0' && ops[i][0] <= '9');
        if (is_num) {
            int64_t v;
            if (parse_u64(ops[i], lens[i], &v) != 0) { err(c, "malformed data value"); return -1; }
            uint8_t b[8];
            for (int k = 0; k < size; k++) b[k] = (uint8_t)((uint64_t)v >> (8 * k));
            sec_put(c, b, (size_t)size);
            continue;
        }
        // symbol reference (only 4- and 8-byte forms are valid)
        if (size != 4 && size != 8) { err(c, "symbol reference requires .int or .quad"); return -1; }
        const char *sym = ops[i];
        size_t slen = lens[i];
        int64_t addend = 0;
        for (size_t k = 0; k < slen; k++)
            if (sym[k] == '+' || sym[k] == '-') {
                if (parse_u64(sym + k, slen - k, &addend) != 0) { err(c, "malformed symbol addend"); return -1; }
                slen = k;
                break;
            }
        int si = sym_intern(c, sym, slen);
        uint8_t b[8];
        for (int x = 0; x < 8; x++) b[x] = (uint8_t)((uint64_t)addend >> (8 * x));
        sec_put(c, b, (size_t)size);
        add_reloc(c, c->cur, (int32_t)u->sec[c->cur].len - size, si,
                  (unsigned)(size == 8 ? 3 : 2),
                  size == 8 ? ASM_RELOC_RISCV_64 : ASM_RELOC_RISCV_32, addend, -1);
    }
    return 0;
}

static int assemble_directive(Ctx *c, const char *line, size_t len) {
    AsmUnit *u = c->u;
    size_t n = 0;
    while (n < len && line[n] != ' ' && line[n] != '\t') n++;
    const char *rest = line + n;
    size_t rest_len = len - n;
    rest = trim(rest, &rest_len);

    if (n == 5 && memcmp(line, ".text", 5) == 0) { c->cur = ASM_SEC_TEXT; u->sec[ASM_SEC_TEXT].used = 1; return 0; }
    if (n == 5 && memcmp(line, ".data", 5) == 0) { c->cur = ASM_SEC_DATA; u->sec[ASM_SEC_DATA].used = 1; return 0; }
    if (n == 4 && memcmp(line, ".bss", 4) == 0) { c->cur = ASM_SEC_BSS; u->sec[ASM_SEC_BSS].used = 1; return 0; }
    if (n == 7 && memcmp(line, ".rodata", 7) == 0) { c->cur = ASM_SEC_RODATA; u->sec[ASM_SEC_RODATA].used = 1; return 0; }

    // ELF metadata directives emitted by QBE in Gaself mode: accepted/ignored.
    if (n == 5 && memcmp(line, ".type", 5) == 0) return 0;
    if (n == 5 && memcmp(line, ".size", 5) == 0) return 0;
    if (n == 8 && memcmp(line, ".section", 8) == 0) {
        if (rest_len >= 15 && memcmp(rest, ".note.GNU-stack", 15) == 0) {
            u->has_gnu_stack = 1;
            return 0;
        }
        if (rest_len >= 5 && memcmp(rest, ".data", 5) == 0) { c->cur = ASM_SEC_DATA; u->sec[ASM_SEC_DATA].used = 1; return 0; }
        if (rest_len >= 4 && memcmp(rest, ".bss", 4) == 0) { c->cur = ASM_SEC_BSS; u->sec[ASM_SEC_BSS].used = 1; return 0; }
        if (rest_len >= 7 && memcmp(rest, ".rodata", 7) == 0) { c->cur = ASM_SEC_RODATA; u->sec[ASM_SEC_RODATA].used = 1; return 0; }
        if (rest_len >= 5 && memcmp(rest, ".text", 5) == 0) { c->cur = ASM_SEC_TEXT; u->sec[ASM_SEC_TEXT].used = 1; return 0; }
    }

    if (n == 6 && memcmp(line, ".globl", 6) == 0) {
        if (rest_len == 0) { err(c, ".globl without symbol name"); return -1; }
        int si = sym_intern(c, rest, rest_len);
        u->syms[si].global = 1;
        return 0;
    }
    if (n == 7 && memcmp(line, ".balign", 7) == 0) {
        int64_t a;
        if (parse_u64(rest, rest_len, &a) != 0 || a <= 0 || (a & (a - 1)) != 0) {
            err(c, "unsupported .balign value");
            return -1;
        }
        sec_align(c, (size_t)a);
        return 0;
    }
    if (n == 6 && memcmp(line, ".ascii", 6) == 0) return parse_ascii(c, rest, rest_len);
    if (n == 5 && memcmp(line, ".byte", 5) == 0) return assemble_data_values(c, 1, rest, rest_len);
    if (n == 6 && memcmp(line, ".short", 6) == 0) return assemble_data_values(c, 2, rest, rest_len);
    if (n == 4 && memcmp(line, ".int", 4) == 0) return assemble_data_values(c, 4, rest, rest_len);
    if (n == 5 && memcmp(line, ".quad", 5) == 0) return assemble_data_values(c, 8, rest, rest_len);
    if (n == 5 && memcmp(line, ".fill", 5) == 0) {
        const char *fops[4];
        size_t flens[4];
        int cnt = split_ops(rest, rest_len, fops, flens);
        int64_t count, one, zero;
        if (cnt != 3 || parse_u64(fops[0], flens[0], &count) != 0 || count < 0 ||
            parse_u64(fops[1], flens[1], &one) != 0 || one != 1 ||
            parse_u64(fops[2], flens[2], &zero) != 0 || zero != 0) {
            err(c, "unsupported .fill form");
            return -1;
        }
        uint8_t b = 0;
        for (int64_t i = 0; i < count; i++) sec_put(c, &b, 1);
        return 0;
    }
    err(c, "unsupported directive '%.*s'", (int)n, line);
    return -1;
}

// Strip /* ... */ comments (QBE emits single-line comments around data).
static void strip_comment(char *line) {
    char *p = strstr(line, "/*");
    if (!p) return;
    char *q = strstr(p, "*/");
    if (q) memmove(p, q + 2, strlen(q + 2) + 1);
    else *p = '\0';
}

// --- fixups and entry point ------------------------------------------------------

static int resolve_fixups(Ctx *c) {
    for (size_t i = 0; i < c->nfixup; i++) {
        Fixup *f = &c->fixups[i];
        Label *lb = label_find(c, f->label, strlen(f->label));
        if (!lb || lb->section != ASM_SEC_TEXT) {
            c->line = f->line;
            err(c, "undefined branch target '%s'", f->label);
            return -1;
        }
        int64_t delta = lb->offset - f->offset;
        uint8_t *ins = c->u->sec[ASM_SEC_TEXT].bytes + f->offset;
        uint32_t word = (uint32_t)ins[0] | ((uint32_t)ins[1] << 8) |
                        ((uint32_t)ins[2] << 16) | ((uint32_t)ins[3] << 24);
        if (f->kind == 0) {
            // B-type branch (±4 KiB, even)
            if (delta & 1) { c->line = f->line; err(c, "misaligned branch target"); return -1; }
            if (delta < -4096 || delta >= 4096) {
                c->line = f->line;
                err(c, "conditional branch target out of range");
                return -1;
            }
            uint32_t i = (uint32_t)delta & 0x1FFF;
            word &= ~0xFE000F80u;
            word |= ((i >> 12) << 31) | (((i >> 5) & 0x3F) << 25) |
                    (((i >> 1) & 0xF) << 8) | (((i >> 11) & 1) << 7);
        } else {
            // J-type jump (±1 MiB, even)
            if (delta & 1) { c->line = f->line; err(c, "misaligned jump target"); return -1; }
            if (delta < -(1 << 20) || delta >= (1 << 20)) {
                c->line = f->line;
                err(c, "jump target out of range");
                return -1;
            }
            uint32_t i = (uint32_t)delta & 0x1FFFFF;
            word &= ~0xFFFFF000u;
            word |= ((i >> 20) << 31) | (((i >> 1) & 0x3FF) << 21) |
                    (((i >> 11) & 1) << 20) | (((i >> 12) & 0xFF) << 12);
        }
        ins[0] = (uint8_t)word;
        ins[1] = (uint8_t)(word >> 8);
        ins[2] = (uint8_t)(word >> 16);
        ins[3] = (uint8_t)(word >> 24);
    }
    return 0;
}

static void assemble_line(Ctx *c, char *line, size_t len) {
    if (c->failed) return;
    char *s = line;
    size_t n = len;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    if (n == 0) return;

    if (s[n - 1] == ':') {
        // label definition
        size_t nl = n - 1;
        while (nl && (s[nl - 1] == ' ' || s[nl - 1] == '\t')) nl--;
        if (nl == 0) { err(c, "empty label"); return; }
        if (c->cur < 0) { err(c, "label '%.*s' outside any section", (int)nl, s); return; }
        int si = sym_find(c->u, s, nl);
        if (si >= 0 && c->u->syms[si].section != -1) {
            err(c, "duplicate symbol '%.*s'", (int)nl, s);
            return;
        }
        si = sym_intern(c, s, nl);
        c->u->syms[si].section = c->cur;
        c->u->syms[si].value = (int64_t)c->u->sec[c->cur].len;
        label_add(c, s, nl, c->cur, (int64_t)c->u->sec[c->cur].len);
        return;
    }

    if (s[0] == '.') {
        assemble_directive(c, s, n);
        return;
    }

    // instruction: mnemonic followed by operands
    size_t m = 0;
    while (m < n && s[m] != ' ' && s[m] != '\t') m++;
    if (m == 0) { err(c, "empty statement"); return; }
    const char *rest = s + m;
    size_t rl = n - m;
    rest = trim(rest, &rl);
    assemble_insn(c, s, m, rest, rl);
}

int asm_rv64_assemble(AsmUnit *u, const char *text, size_t len) {
    if (u->fmt != ASM_FMT_ELF) {
        snprintf(u->errmsg, sizeof(u->errmsg),
                 "asm_rv64: output format must be ASM_FMT_ELF");
        u->errline = 1;
        u->has_error = 1;
        return 1;
    }
    u->has_error = 0;
    u->errline = 0;
    u->machine = EM_RISCV;

    Ctx c;
    memset(&c, 0, sizeof(c));
    c.u = u;
    c.cur = -1;
    c.line = 1;

    char *buf = malloc(len + 1);
    if (!buf) {
        snprintf(u->errmsg, sizeof(u->errmsg), "asm_rv64: out of memory");
        u->errline = 1;
        u->has_error = 1;
        return 1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    char *p = buf;
    while (*p && !c.failed) {
        char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        char saved = p[llen];
        p[llen] = '\0';
        strip_comment(p);
        assemble_line(&c, p, strlen(p));
        p[llen] = saved;
        if (!nl) break;
        p = nl + 1;
        c.line++;
    }

    if (!c.failed)
        resolve_fixups(&c);

    for (size_t i = 0; i < c.nlabel; i++) free(c.labels[i].name);
    free(c.labels);
    for (size_t i = 0; i < c.nfixup; i++) free(c.fixups[i].label);
    free(c.fixups);
    free(buf);

    return u->has_error ? 1 : 0;
}
