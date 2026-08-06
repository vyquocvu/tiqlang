// M17.3.1: integrated arm64 assembler for the QBE assembly subset.
// M17.3.3: extended with ELF output mode for aarch64 Linux.
//
// QBE (third_party/qbe, arm64 target, Gasmacho or Gaself flavor) emits
// a small, stable subset of gas-style arm64 assembly (see arm64/emit.c
// omap[] and gas.c). This module parses exactly that subset and
// produces section contents plus relocations in memory. Anything outside
// the subset fails closed with a located diagnostic; partial objects
// are never produced.

#include "../include/asm_arm64.h"
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
    int64_t offset;  // of the branch instruction
    int line;
    int cond;        // 1 for b.<cond>, 0 for b
    char *label;     // owned
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

// --- error reporting ---------------------------------------------------------

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

// --- unit / section helpers ---------------------------------------------------

void asm_unit_init(AsmUnit *u) {
    memset(u, 0, sizeof(*u));
}

void asm_unit_free(AsmUnit *u) {
    for (int i = 0; i < ASM_SEC_COUNT; i++) {
        free(u->sec[i].bytes);
        free(u->sec[i].relocs);
    }
    for (size_t i = 0; i < u->nsym; i++) free(u->syms[i].name);
    free(u->syms);
    memset(u, 0, sizeof(*u));
}

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

// Pad the current section with zeros up to a power-of-two alignment.
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

// --- symbol / label tables ----------------------------------------------------

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

static int label_is_local(const char *name, size_t len) {
    (void)len;
    // QBE prefixes every exported/data symbol with '_' in Mach-O mode;
    // labels starting with 'L' or ".L" are assembler-local.
    return name[0] == 'L' || name[0] == '.';
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

static void fixup_add(Ctx *c, int64_t offset, int cond, const char *label, size_t len) {
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
    c->fixups[c->nfixup].cond = cond;
    c->fixups[c->nfixup].label = copy;
    c->nfixup++;
}

// --- token helpers -------------------------------------------------------------

// Trim leading/trailing whitespace in place; returns start, sets *len.
static const char *trim(const char *s, size_t *len) {
    size_t n = *len;
    while (n && (s[0] == ' ' || s[0] == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    *len = n;
    return s;
}

typedef struct {
    int cls;  // 'x', 'w', 's', 'd', 'q'
    int idx;  // 0..31 (sp == x31, xzr/wzr == 31)
} Reg;

static int parse_reg(const char *s, size_t len, Reg *r) {
    if (len == 2 && s[0] == 's' && s[1] == 'p') { r->cls = 'x'; r->idx = 31; return 0; }
    if (len == 3 && (s[1] == 'z' && s[2] == 'r') && (s[0] == 'x' || s[0] == 'w')) {
        r->cls = s[0]; r->idx = 31; return 0;
    }
    if (len < 2 || len > 3) return -1;
    char cls = s[0];
    if (cls != 'x' && cls != 'w' && cls != 's' && cls != 'd' && cls != 'q') return -1;
    int n = 0;
    for (size_t i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    int max = (cls == 'x' || cls == 'w') ? 30 : 31;
    if (n > max) return -1;
    r->cls = cls;
    r->idx = n;
    return 0;
}

static int parse_int(const char *s, size_t len, int64_t *out) {
    if (len == 0) return -1;
    int neg = 0;
    size_t i = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    else if (s[0] == '+') i = 1;
    if (i >= len) return -1;
    int base = 10;
    if (len - i >= 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    if (i >= len) return -1;
    int64_t v = 0;
    for (; i < len; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return -1;
        v = v * base + d;
    }
    *out = neg ? -v : v;
    return 0;
}

// Parse "#N" (with optional '#'); returns 0 on success.
static int parse_imm(const char *s, size_t len, int64_t *out) {
    if (len && s[0] == '#') { s++; len--; }
    return parse_int(s, len, out);
}

// Memory operand forms produced by QBE:
//   [base]            plain (offset 0)
//   [base, off]       unsigned offset (with or without '#')
//   [base, off]!      pre-indexed (stp)
//   [base], off       post-indexed (ldp) -- caller passes the trailing
//                     offset as a separate operand
typedef struct {
    Reg base;
    int64_t off;
    int bang;
} Mem;

static int parse_mem(const char *s, size_t len, Mem *m) {
    if (len < 3 || s[0] != '[') return -1;
    size_t end = len;
    m->bang = 0;
    if (s[len - 1] == '!') { m->bang = 1; end--; }
    if (s[end - 1] != ']') return -1;
    end--;
    const char *p = s + 1;
    size_t n = end - 1;
    // split base / offset at the top-level comma
    size_t comma = n;
    for (size_t i = 0; i < n; i++)
        if (p[i] == ',') { comma = i; break; }
    size_t blen = comma;
    const char *base = trim(p, &blen);
    if (parse_reg(base, blen, &m->base) != 0) return -1;
    m->off = 0;
    if (comma < n) {
        const char *os = p + comma + 1;
        size_t olen = n - comma - 1;
        os = trim(os, &olen);
        if (parse_imm(os, olen, &m->off) != 0) return -1;
    }
    return 0;
}

// Split the operand part of an instruction line at top-level commas.
// Returns operand count, fills ops/lens (max 5).
static int split_ops(const char *s, size_t len, const char **ops, size_t *lens) {
    int count = 0;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        char ch = i < len ? s[i] : ',';
        if (ch == '[') depth++;
        else if (ch == ']') depth--;
        else if (ch == ',' && depth == 0) {
            if (count >= 5) return -1;
            size_t l = i - start;
            const char *p = trim(s + start, &l);
            ops[count] = p;
            lens[count] = l;
            count++;
            start = i + 1;
            if (i == len) break;
        }
    }
    // a trailing empty operand ("a,") is rejected by the caller's parsers
    return count;
}

// --- condition codes -----------------------------------------------------------

static int cond_code(const char *s, size_t len) {
    static const char *names[] = {
        "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
        "hi", "ls", "ge", "lt", "gt", "le", "hs", "lo"
    };
    for (int i = 0; i < 16; i++)
        if (strlen(names[i]) == len && memcmp(s, names[i], len) == 0)
            return i < 14 ? i : (i == 14 ? 2 : 3);
    return -1;
}

// --- instruction emitters --------------------------------------------------------

static int req_text(Ctx *c) {
    if (c->cur != ASM_SEC_TEXT) {
        err(c, "instruction outside .text section");
        return -1;
    }
    return 0;
}

// mov Xd/Wd, #imm: decompose into movz/movn/movk (semantics identical to
// the gas mov alias; no logical-immediate encoder needed).
static void emit_mov_imm(Ctx *c, Reg rd, int64_t val) {
    if (rd.cls == 'x') {
        uint64_t v = (uint64_t)val;
        if ((v & ~(uint64_t)0xFFFF) == 0) {
            sec_put32(c, 0xD2800000u | ((uint32_t)(v & 0xFFFF) << 5) | (uint32_t)rd.idx);
        } else if ((~v & ~(uint64_t)0xFFFF) == 0) {
            sec_put32(c, 0x92800000u | ((uint32_t)(~v & 0xFFFF) << 5) | (uint32_t)rd.idx);
        } else {
            sec_put32(c, 0xD2800000u | ((uint32_t)(v & 0xFFFF) << 5) | (uint32_t)rd.idx);
            for (int hw = 1; hw < 4; hw++) {
                uint32_t h = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
                if (h) sec_put32(c, 0xF2800000u | ((uint32_t)hw << 21) | (h << 5) | (uint32_t)rd.idx);
            }
        }
    } else { // 'w'
        uint32_t v = (uint32_t)val;
        if ((v & 0xFFFF0000u) == 0) {
            sec_put32(c, 0x52800000u | ((v & 0xFFFF) << 5) | (uint32_t)rd.idx);
        } else if ((~v & 0xFFFF0000u) == 0) {
            sec_put32(c, 0x12800000u | (((~v >> 16) & 0xFFFF) << 5) | (uint32_t)rd.idx);
        } else {
            sec_put32(c, 0x52800000u | ((v & 0xFFFF) << 5) | (uint32_t)rd.idx);
            sec_put32(c, 0x72800000u | (1u << 21) | (((v >> 16) & 0xFFFF) << 5) | (uint32_t)rd.idx);
        }
    }
}

// Encode a pc-relative branch to a label; immediate patched later via fixup.
static void emit_branch_local(Ctx *c, int cond) {
    if (cond < 0) sec_put32(c, 0x14000000u);
    else sec_put32(c, 0x54000000u | (uint32_t)cond);
}

// --- instruction dispatch ----------------------------------------------------------

// Encode one instruction line. `mn`/`mnlen` is the mnemonic, ops/lens the
// operands (already trimmed). Returns 0 on success.
static int assemble_insn(Ctx *c, const char *mn, size_t mnlen,
                         const char **ops, size_t *lens, int nops) {
    AsmUnit *u = c->u;
    if (req_text(c) != 0) return -1;

    // --- three-register ALU forms ------------------------------------------
    // (add/sub are handled below together with their immediate forms)
    struct { const char *name; uint32_t base; } alu3[] = {
        { "and", 0x8A000000u }, { "orr", 0xAA000000u }, { "eor", 0xCA000000u },
        { "lsl", 0x9AC20000u }, { "lsr", 0x9AC22400u }, { "asr", 0x9AC22800u },
        { "sdiv", 0x9AC00C00u }, { "udiv", 0x9AC00800u },
    };
    for (size_t i = 0; i < sizeof(alu3) / sizeof(alu3[0]); i++) {
        if (mnlen == strlen(alu3[i].name) && memcmp(mn, alu3[i].name, mnlen) == 0) {
            Reg rd, rn, rm;
            if (nops != 3 || parse_reg(ops[0], lens[0], &rd) ||
                parse_reg(ops[1], lens[1], &rn) || parse_reg(ops[2], lens[2], &rm) ||
                rd.cls != 'x' || rn.cls != 'x' || rm.cls != 'x') {
                err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
                return -1;
            }
            uint32_t base = alu3[i].base;
            // add/sub (shifted register) place Rm at bits 20:16 like the
            // data-processing encoding; lsl/lsr/asr/sdiv/udiv share it.
            sec_put32(c, base | ((uint32_t)rm.idx << 16) | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            return 0;
        }
    }

    // --- add/sub/cmp/cmn with immediate or register operand -------------------
    // gas forms: add/sub `op rd, rn, op2` (3 operands), cmp/cmn write the
    // flags so their form is `op rn, op2` (2 operands).
    if (mnlen == 3 && (memcmp(mn, "add", 3) == 0 || memcmp(mn, "sub", 3) == 0 ||
                       memcmp(mn, "cmp", 3) == 0 || memcmp(mn, "cmn", 3) == 0)) {
        int is_sub = mn[2] == 'b';
        int is_cmp = mn[0] == 'c';
        if (nops < (is_cmp ? 2 : 3)) {
            err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
            return -1;
        }
        Reg rd, rn;
        int oi; // operand index of rn
        if (is_cmp) { rd.cls = 'x'; rd.idx = 31; oi = 0; }
        else {
            if (parse_reg(ops[0], lens[0], &rd) != 0 || rd.cls != 'x') {
                err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
                return -1;
            }
            oi = 1;
        }
        if (parse_reg(ops[oi], lens[oi], &rn) != 0 || rn.cls != 'x') {
            err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
            return -1;
        }
        int si = oi + 1; // operand index of the immediate / second register
        if (lens[si] > 14 && memcmp(ops[si], "#:lo12:", 7) == 0 &&
            memcmp(ops[si] + lens[si] - 8, "@PAGEOFF", 8) == 0) {
            // add xd, xn, #:lo12:sym@PAGEOFF (page-offset relocation)
            if (is_cmp || is_sub || nops != si + 1) {
                err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
                return -1;
            }
            int symi = sym_intern(c, ops[si] + 7, lens[si] - 7 - 8);
            sec_put32(c, 0x91000000u | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            add_reloc(c, ASM_SEC_TEXT, (int32_t)u->sec[ASM_SEC_TEXT].len - 4, symi, 0, 2,
                      ASM_RELOC_PAGEOFF12);
            return 0;
        }
        if (lens[si] > 0 && ops[si][0] == '#') {
            // immediate form (optional `lsl #12` shift)
            int64_t imm;
            if (parse_imm(ops[si], lens[si], &imm) != 0 || imm < 0 || imm > 4095) {
                // negative immediates reach us as cmn via QBE; reject otherwise
                err(c, "immediate out of range in %.*s", (int)mnlen, mn);
                return -1;
            }
            uint32_t sh = 0;
            if (nops == si + 2) {
                if (lens[si + 1] != 8 || memcmp(ops[si + 1], "lsl #12", 7) != 0) {
                    err(c, "unsupported shift in %.*s", (int)mnlen, mn);
                    return -1;
                }
                sh = 1;
            } else if (nops != si + 1) {
                err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
                return -1;
            }
            uint32_t base;
            if (is_cmp) base = mn[2] == 'n' ? 0xB1000000u : 0xF1000000u; // cmn=adds, cmp=subs
            else if (is_sub) base = 0xD1000000u;
            else base = 0x91000000u;
            sec_put32(c, base | (sh << 22) | ((uint32_t)imm << 10) |
                         ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            return 0;
        }
        if (nops == si + 1) {
            // register form
            Reg rm;
            if (parse_reg(ops[si], lens[si], &rm) != 0 || rm.cls != 'x') {
                err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
                return -1;
            }
            uint32_t base;
            if (is_cmp) base = mn[2] == 'n' ? 0xAB000000u : 0xEB000000u; // cmn=adds, cmp=subs
            else if (is_sub) base = 0xCB000000u;
            else base = 0x8B000000u;
            sec_put32(c, base | ((uint32_t)rm.idx << 16) |
                         ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            return 0;
        }
        err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
        return -1;
    }

    // --- neg / mul / msub -----------------------------------------------------
    if (mnlen == 3 && memcmp(mn, "neg", 3) == 0) {
        Reg rd, rm;
        if (nops != 2 || parse_reg(ops[0], lens[0], &rd) || parse_reg(ops[1], lens[1], &rm) ||
            rd.cls != 'x' || rm.cls != 'x') { err(c, "unsupported operand form for neg"); return -1; }
        sec_put32(c, 0xCB0003E0u | ((uint32_t)rm.idx << 16) | (uint32_t)rd.idx);
        return 0;
    }
    if (mnlen == 3 && memcmp(mn, "mul", 3) == 0) {
        Reg rd, rn, rm;
        if (nops != 3 || parse_reg(ops[0], lens[0], &rd) || parse_reg(ops[1], lens[1], &rn) ||
            parse_reg(ops[2], lens[2], &rm) || rd.cls != 'x' || rn.cls != 'x' || rm.cls != 'x') {
            err(c, "unsupported operand form for mul"); return -1;
        }
        sec_put32(c, 0x9B007C00u | ((uint32_t)rm.idx << 16) | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
        return 0;
    }
    if (mnlen == 4 && memcmp(mn, "msub", 4) == 0) {
        Reg rd, rn, rm, ra;
        if (nops != 4 || parse_reg(ops[0], lens[0], &rd) || parse_reg(ops[1], lens[1], &rn) ||
            parse_reg(ops[2], lens[2], &rm) || parse_reg(ops[3], lens[3], &ra) ||
            rd.cls != 'x' || rn.cls != 'x' || rm.cls != 'x' || ra.cls != 'x') {
            err(c, "unsupported operand form for msub"); return -1;
        }
        sec_put32(c, 0x9B008000u | ((uint32_t)rm.idx << 16) | ((uint32_t)ra.idx << 10) |
                     ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
        return 0;
    }

    // --- mov / movk --------------------------------------------------------------
    if (mnlen == 3 && memcmp(mn, "mov", 3) == 0) {
        if (nops != 2) { err(c, "unsupported operand form for mov"); return -1; }
        Reg rd;
        if (parse_reg(ops[0], lens[0], &rd) != 0) { err(c, "bad register in mov"); return -1; }
        if (lens[1] > 0 && ops[1][0] == '#') {
            int64_t imm;
            if (parse_imm(ops[1], lens[1], &imm) != 0 || (rd.cls != 'x' && rd.cls != 'w')) {
                err(c, "unsupported immediate in mov"); return -1;
            }
            emit_mov_imm(c, rd, imm);
            return 0;
        }
        Reg rn;
        if (parse_reg(ops[1], lens[1], &rn) != 0) { err(c, "bad register in mov"); return -1; }
        if (rd.cls == rn.cls && (rd.cls == 'x' || rd.cls == 'w')) {
            // mov involving sp must use the add alias (sp is not in the
            // extended register encoding of orr)
            if (rd.idx == 31 || rn.idx == 31) {
                sec_put32(c, 0x91000000u | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            } else {
                uint32_t base = rd.cls == 'x' ? 0xAA0003E0u : 0x2A0003E0u;
                sec_put32(c, base | ((uint32_t)rn.idx << 16) | (uint32_t)rd.idx);
            }
            return 0;
        }
        if ((rd.cls == 's' && rn.cls == 's') || (rd.cls == 'd' && rn.cls == 'd')) {
            uint32_t base = rd.cls == 's' ? 0x1E204000u : 0x1E604000u;
            sec_put32(c, base | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            return 0;
        }
        err(c, "unsupported operand form for mov");
        return -1;
    }
    if (mnlen == 4 && memcmp(mn, "movk", 4) == 0) {
        Reg rd;
        int64_t imm, sh = 0;
        if (nops != 3 || parse_reg(ops[0], lens[0], &rd) != 0 || rd.cls != 'x' ||
            parse_imm(ops[1], lens[1], &imm) != 0 || imm < 0 || imm > 0xFFFF ||
            lens[2] < 7 || memcmp(ops[2], "lsl #", 5) != 0 || parse_int(ops[2] + 5, lens[2] - 5, &sh) != 0 ||
            (sh != 0 && sh != 16 && sh != 32 && sh != 48)) {
            err(c, "unsupported operand form for movk"); return -1;
        }
        sec_put32(c, 0xF2800000u | ((uint32_t)(sh / 16) << 21) |
                     ((uint32_t)imm << 5) | (uint32_t)rd.idx);
        return 0;
    }

    // --- adrp / add lo12 (symbol address materialization) -----------------------
    if (mnlen == 4 && memcmp(mn, "adrp", 4) == 0) {
        Reg rd;
        if (nops != 2 || parse_reg(ops[0], lens[0], &rd) != 0 || rd.cls != 'x') {
            err(c, "unsupported operand form for adrp"); return -1;
        }
        const char *sym = ops[1];
        size_t slen = lens[1];
        if (slen <= 5 || memcmp(sym + slen - 5, "@PAGE", 5) != 0) {
            err(c, "adrp requires a @PAGE symbol operand"); return -1;
        }
        slen -= 5;
        int si = sym_intern(c, sym, slen);
        sec_put32(c, 0x90000000u | (uint32_t)rd.idx);
        add_reloc(c, ASM_SEC_TEXT, (int32_t)u->sec[ASM_SEC_TEXT].len - 4, si, 1, 2,
                  ASM_RELOC_PAGE21);
        return 0;
    }

    // --- loads and stores ---------------------------------------------------------
    struct { const char *name; uint32_t base; int scale; int rcls; } ls[] = {
        { "str",   0xF9000000u, 8, 'x' }, { "ldr",   0xF9400000u, 8, 'x' },
        { "ldrsw", 0xB9800000u, 4, 'x' },
        { "strh",  0x79000000u, 2, 'w' }, { "ldrh",  0x79400000u, 2, 'w' },
        { "strb",  0x39000000u, 1, 'w' }, { "ldrb",  0x39400000u, 1, 'w' },
        { "ldrsb", 0x39800000u, 1, 'x' }, { "ldrsh", 0x79800000u, 2, 'x' },
        { "str",   0xBD000000u, 4, 's' }, { "ldr",   0xBD400000u, 4, 's' },
        { "str",   0xFD000000u, 8, 'd' }, { "ldr",   0xFD400000u, 8, 'd' },
    };
    for (size_t i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) {
        if (mnlen == strlen(ls[i].name) && memcmp(mn, ls[i].name, mnlen) == 0) {
            Reg rt;
            Mem m;
            if (nops != 2 || parse_reg(ops[0], lens[0], &rt) != 0 ||
                rt.cls != ls[i].rcls || parse_mem(ops[1], lens[1], &m) != 0 ||
                m.bang || m.base.cls != 'x') continue;
            if (m.off < 0 || m.off % ls[i].scale != 0 || m.off / ls[i].scale > 4095) {
                err(c, "offset out of range in %.*s", (int)mnlen, mn);
                return -1;
            }
            sec_put32(c, ls[i].base | ((uint32_t)(m.off / ls[i].scale) << 10) |
                         ((uint32_t)m.base.idx << 5) | (uint32_t)rt.idx);
            return 0;
        }
    }

    // --- stp (pre-indexed) / ldp (post-indexed) ------------------------------------
    if (mnlen == 3 && (memcmp(mn, "stp", 3) == 0 || memcmp(mn, "ldp", 3) == 0)) {
        int store = mn[1] == 't';
        Reg r1, r2;
        if (nops != (store ? 3 : 4) || parse_reg(ops[0], lens[0], &r1) != 0 ||
            parse_reg(ops[1], lens[1], &r2) != 0 || r1.cls != 'x' || r2.cls != 'x') {
            err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
            return -1;
        }
        Mem m;
        if (parse_mem(ops[2], lens[2], &m) != 0 || m.base.cls != 'x') {
            err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
            return -1;
        }
        if (store) {
            // stp Xt, Xt2, [Xn, #imm]!  (imm = -512..504, multiple of 8)
            if (!m.bang || m.off < -512 || m.off > 504 || m.off % 8 != 0) {
                err(c, "unsupported stp addressing mode");
                return -1;
            }
            uint32_t imm7 = (uint32_t)((m.off / 8) & 0x7F);
            sec_put32(c, 0xA9800000u | (imm7 << 15) | ((uint32_t)r2.idx << 10) |
                         ((uint32_t)m.base.idx << 5) | (uint32_t)r1.idx);
            return 0;
        }
        // ldp Xt, Xt2, [Xn], #imm  (post-index; trailing offset operand)
        int64_t off;
        if (m.bang || m.off != 0 || parse_imm(ops[3], lens[3], &off) != 0 ||
            off < -512 || off > 504 || off % 8 != 0) {
            err(c, "unsupported ldp addressing mode");
            return -1;
        }
        uint32_t imm7 = (uint32_t)((off / 8) & 0x7F);
        sec_put32(c, 0xA8C00000u | (imm7 << 15) | ((uint32_t)r2.idx << 10) |
                     ((uint32_t)m.base.idx << 5) | (uint32_t)r1.idx);
        return 0;
    }

    // --- branches / calls -----------------------------------------------------------
    if (mnlen == 2 && memcmp(mn, "bl", 2) == 0) {
        if (nops != 1) { err(c, "unsupported operand form for bl"); return -1; }
        Label *lb = label_find(c, ops[0], lens[0]);
        if (lb && lb->section == ASM_SEC_TEXT) {
            int64_t delta = (lb->offset - (int64_t)u->sec[ASM_SEC_TEXT].len) / 4;
            sec_put32(c, 0x94000000u | ((uint32_t)delta & 0x03FFFFFFu));
            return 0;
        }
        int si = sym_intern(c, ops[0], lens[0]);
        sec_put32(c, 0x94000000u);
        add_reloc(c, ASM_SEC_TEXT, (int32_t)u->sec[ASM_SEC_TEXT].len - 4, si, 1, 2,
                  ASM_RELOC_BRANCH26);
        return 0;
    }
    if (mn[0] == 'b' && (mnlen == 1 || cond_code(mn + 1, mnlen - 1) >= 0)) {
        // bare "b" or "b<cond>" (blt/ble/bls are b.lt/b.le/b.ls); exact
        // "bl" returned above and "blr" has no matching condition code
        int cond = mnlen == 1 ? -1 : cond_code(mn + 1, mnlen - 1);
        if (nops != 1) { err(c, "unsupported operand form for branch"); return -1; }
        Label *lb = label_find(c, ops[0], lens[0]);
        if (lb && lb->section == ASM_SEC_TEXT) {
            int64_t delta = (lb->offset - (int64_t)u->sec[ASM_SEC_TEXT].len) / 4;
            if (cond < 0) sec_put32(c, 0x14000000u | ((uint32_t)delta & 0x03FFFFFFu));
            else sec_put32(c, 0x54000000u | (((uint32_t)delta & 0x7FFFFu) << 5) | (uint32_t)cond);
            return 0;
        }
        fixup_add(c, (int64_t)u->sec[ASM_SEC_TEXT].len, cond >= 0, ops[0], lens[0]);
        emit_branch_local(c, cond);
        return 0;
    }
    if (mnlen == 3 && memcmp(mn, "blr", 3) == 0) {
        Reg rn;
        if (nops != 1 || parse_reg(ops[0], lens[0], &rn) != 0 || rn.cls != 'x') {
            err(c, "unsupported operand form for blr"); return -1;
        }
        sec_put32(c, 0xD63F0000u | ((uint32_t)rn.idx << 5));
        return 0;
    }
    if (mnlen == 3 && memcmp(mn, "ret", 3) == 0) {
        if (nops != 0) { err(c, "unsupported operand form for ret"); return -1; }
        sec_put32(c, 0xD65F03C0u);
        return 0;
    }

    // --- sign/zero extension ----------------------------------------------------------
    struct { const char *name; uint32_t base; int dcls; } ext[] = {
        { "sxtb", 0x93401C00u, 'x' }, { "sxth", 0x93403C00u, 'x' },
        { "sxtw", 0x93407C00u, 'x' }, { "uxtb", 0x53001C00u, 'w' },
        { "uxth", 0x53003C00u, 'w' },
    };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        if (mnlen == 4 && memcmp(mn, ext[i].name, 4) == 0) {
            Reg rd, rn;
            if (nops != 2 || parse_reg(ops[0], lens[0], &rd) != 0 || rd.cls != ext[i].dcls ||
                parse_reg(ops[1], lens[1], &rn) != 0 || rn.cls != 'w') {
                err(c, "unsupported operand form for %.*s", (int)mnlen, mn);
                return -1;
            }
            sec_put32(c, ext[i].base | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
            return 0;
        }
    }

    // --- cset ---------------------------------------------------------------------------
    if (mnlen == 4 && memcmp(mn, "cset", 4) == 0) {
        Reg rd;
        int cc;
        if (nops != 2 || parse_reg(ops[0], lens[0], &rd) != 0 ||
            (rd.cls != 'w' && rd.cls != 'x') || (cc = cond_code(ops[1], lens[1])) < 0) {
            err(c, "unsupported operand form for cset"); return -1;
        }
        int inv = (cc & ~1) | ((cc & 1) ^ 1);
        uint32_t base = rd.cls == 'w' ? 0x1A9F07E0u : 0x9A9F07E0u;
        sec_put32(c, base | ((uint32_t)inv << 12) | (uint32_t)rd.idx);
        return 0;
    }

    // --- floating point --------------------------------------------------------------------
    if (nops >= 2) {
        Reg rd, rn;
        int fp2 = parse_reg(ops[0], lens[0], &rd) == 0 && parse_reg(ops[1], lens[1], &rn) == 0 &&
                  (rd.cls == 's' || rd.cls == 'd') && rn.cls == rd.cls;
        if (fp2) {
            int d = rd.cls == 'd';
            uint32_t t = d ? 0x400000u : 0;
            if (mnlen == 4 && memcmp(mn, "fadd", 4) == 0 && nops == 3) goto fp3;
            if (mnlen == 4 && memcmp(mn, "fsub", 4) == 0 && nops == 3) goto fp3;
            if (mnlen == 4 && memcmp(mn, "fmul", 4) == 0 && nops == 3) goto fp3;
            if (mnlen == 4 && memcmp(mn, "fdiv", 4) == 0 && nops == 3) goto fp3;
            if (mnlen == 4 && memcmp(mn, "fneg", 4) == 0 && nops == 2) {
                sec_put32(c, (0x1E214000u | t) | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                return 0;
            }
            if (mnlen == 4 && memcmp(mn, "fmov", 4) == 0 && nops == 2) {
                sec_put32(c, (0x1E204000u | t) | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                return 0;
            }
            if (mnlen == 5 && memcmp(mn, "fcmpe", 5) == 0 && nops == 2) {
                sec_put32(c, (0x1E202000u | t) | ((uint32_t)rn.idx << 16) | (uint32_t)rd.idx);
                return 0;
            }
            if (mnlen == 4 && memcmp(mn, "fcvt", 4) == 0 && nops == 2 && rd.cls != rn.cls) {
                uint32_t base = d ? 0x1E624000u : 0x1E22C000u; // d<-s : s<-d
                sec_put32(c, base | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                return 0;
            }
        }
    }
fp3:
    if (nops == 3) {
        Reg rd, rn, rm;
        if (parse_reg(ops[0], lens[0], &rd) == 0 && parse_reg(ops[1], lens[1], &rn) == 0 &&
            parse_reg(ops[2], lens[2], &rm) == 0 &&
            (rd.cls == 's' || rd.cls == 'd') && rn.cls == rd.cls && rm.cls == rd.cls) {
            uint32_t t = rd.cls == 'd' ? 0x400000u : 0;
            uint32_t base = 0;
            if (mnlen == 4 && memcmp(mn, "fadd", 4) == 0) base = 0x1E202800u;
            else if (mnlen == 4 && memcmp(mn, "fsub", 4) == 0) base = 0x1E203800u;
            else if (mnlen == 4 && memcmp(mn, "fmul", 4) == 0) base = 0x1E200800u;
            else if (mnlen == 4 && memcmp(mn, "fdiv", 4) == 0) base = 0x1E201800u;
            if (base) {
                sec_put32(c, (base | t) | ((uint32_t)rm.idx << 16) |
                             ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                return 0;
            }
        }
    }
    // float <-> integer conversions
    if (nops == 2) {
        Reg rd, rn;
        if (parse_reg(ops[0], lens[0], &rd) == 0 && parse_reg(ops[1], lens[1], &rn) == 0) {
            int fi = rd.cls == 's' || rd.cls == 'd';  // dst float?
            int ii = rn.cls == 'x' || rn.cls == 'w';  // src int?
            if (fi && ii) { // scvtf / ucvtf
                uint32_t t = rd.cls == 'd' ? 0x400000u : 0;
                uint32_t sf = rn.cls == 'x' ? 0x80000000u : 0;
                uint32_t base;
                if (mnlen == 5 && memcmp(mn, "scvtf", 5) == 0) base = 0x1E220000u;
                else if (mnlen == 5 && memcmp(mn, "ucvtf", 5) == 0) base = 0x1E230000u;
                else goto fail_insn;
                sec_put32(c, sf | (base | t) | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                return 0;
            }
            if (!fi && (rn.cls == 's' || rn.cls == 'd')) { // fcvtzs / fcvtzu
                uint32_t t = rn.cls == 'd' ? 0x400000u : 0;
                uint32_t sf = rd.cls == 'x' ? 0x80000000u : 0;
                uint32_t base;
                if (mnlen == 6 && memcmp(mn, "fcvtzs", 6) == 0) base = 0x1E380000u;
                else if (mnlen == 6 && memcmp(mn, "fcvtzu", 6) == 0) base = 0x1E390000u;
                else goto fail_insn;
                sec_put32(c, sf | (base | t) | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                return 0;
            }
            if ((rd.cls == 'w' || rd.cls == 'x') && (rn.cls == 's' || rn.cls == 'd')) {
                if (mnlen == 4 && memcmp(mn, "fmov", 4) == 0) { // fp -> gpr
                    uint32_t base;
                    if (rd.cls == 'w' && rn.cls == 's') base = 0x1E260000u;
                    else if (rd.cls == 'x' && rn.cls == 'd') base = 0x9E660000u;
                    else goto fail_insn;
                    sec_put32(c, base | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                    return 0;
                }
            }
            if ((rd.cls == 's' || rd.cls == 'd') && (rn.cls == 'w' || rn.cls == 'x')) {
                if (mnlen == 4 && memcmp(mn, "fmov", 4) == 0) { // gpr -> fp
                    uint32_t base;
                    if (rd.cls == 's' && rn.cls == 'w') base = 0x1E270000u;
                    else if (rd.cls == 'd' && rn.cls == 'x') base = 0x9E670000u;
                    else goto fail_insn;
                    sec_put32(c, base | ((uint32_t)rn.idx << 5) | (uint32_t)rd.idx);
                    return 0;
                }
            }
        }
    }

fail_insn:
    err(c, "unsupported instruction '%.*s'", (int)mnlen, mn);
    return -1;
}

// --- data directives ------------------------------------------------------------------

// Parse a gas string literal with escapes; appends bytes to current section.
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

// .byte/.short/.int/.quad value list; .quad additionally accepts a symbol
// reference (sym, sym+N, sym-N) with an UNSIGNED relocation.
static int assemble_data_values(Ctx *c, int size, const char *rest, size_t rest_len) {
    AsmUnit *u = c->u;
    const char *ops[8];
    size_t lens[8];
    int n = split_ops(rest, rest_len, ops, lens);
    if (n <= 0) { err(c, "missing data value"); return -1; }
    for (int i = 0; i < n; i++) {
        if (lens[i] == 0) { err(c, "missing data value"); return -1; }
        if (ops[i][0] == '-' || (ops[i][0] >= '0' && ops[i][0] <= '9')) {
            int64_t v;
            if (parse_int(ops[i], lens[i], &v) != 0) { err(c, "malformed data value"); return -1; }
            uint8_t b[8];
            for (int k = 0; k < size; k++) b[k] = (uint8_t)((uint64_t)v >> (8 * k));
            sec_put(c, b, (size_t)size);
            continue;
        }
        // symbol reference: only .quad is supported
        if (size != 8) { err(c, "symbol reference requires .quad"); return -1; }
        const char *sym = ops[i];
        size_t slen = lens[i];
        int64_t addend = 0;
        size_t k = 0;
        for (; k < slen; k++)
            if (sym[k] == '+' || sym[k] == '-') break;
        if (k < slen) {
            if (parse_int(sym + k, slen - k, &addend) != 0) {
                err(c, "malformed symbol addend");
                return -1;
            }
            slen = k;
        }
        int si = sym_intern(c, sym, slen);
        uint8_t b[8];
        for (int x = 0; x < 8; x++) b[x] = (uint8_t)((uint64_t)addend >> (8 * x));
        sec_put(c, b, 8);
        add_reloc(c, c->cur, (int32_t)u->sec[c->cur].len - 8, si, 0, 3,
                  ASM_RELOC_UNSIGNED);
    }
    return 0;
}

static int assemble_directive(Ctx *c, const char *line, size_t len) {
    AsmUnit *u = c->u;
    // directive name up to first space
    size_t n = 0;
    while (n < len && line[n] != ' ' && line[n] != '\t') n++;
    const char *rest = line + n;
    size_t rest_len = len - n;
    rest = trim(rest, &rest_len);

    if (n == 5 && memcmp(line, ".text", 5) == 0) { c->cur = ASM_SEC_TEXT; u->sec[ASM_SEC_TEXT].used = 1; return 0; }
    if (n == 5 && memcmp(line, ".data", 5) == 0) { c->cur = ASM_SEC_DATA; u->sec[ASM_SEC_DATA].used = 1; return 0; }
    if (n == 4 && memcmp(line, ".bss", 4) == 0) { c->cur = ASM_SEC_BSS; u->sec[ASM_SEC_BSS].used = 1; return 0; }
    if (n == 7 && memcmp(line, ".rodata", 7) == 0) { c->cur = ASM_SEC_RODATA; u->sec[ASM_SEC_RODATA].used = 1; return 0; }

    // M17.3.3: ELF-specific directives emitted by QBE in Gaself mode.
    // .type and .size are metadata; we parse and accept them but do not
    // need to act on them (the object writer derives the info from the
    // symbol table). .section .note.GNU-stack marks non-exec stack.
    if (u->fmt == ASM_FMT_ELF) {
        if (n == 5 && memcmp(line, ".type", 5) == 0) return 0; // parsed, ignored
        if (n == 5 && memcmp(line, ".size", 5) == 0) return 0; // parsed, ignored
        if (n == 8 && memcmp(line, ".section", 8) == 0) {
            if (rest_len >= 16 && memcmp(rest, ".note.GNU-stack", 15) == 0) {
                u->has_gnu_stack = 1;
                return 0;
            }
            // Other .section directives: check for known data sections
            if (rest_len >= 5 && memcmp(rest, ".data", 5) == 0) {
                c->cur = ASM_SEC_DATA; u->sec[ASM_SEC_DATA].used = 1; return 0;
            }
            if (rest_len >= 6 && memcmp(rest, ".bss", 4) == 0 &&
                (rest_len == 4 || rest[4] == ',' || rest[4] == ' ')) {
                c->cur = ASM_SEC_BSS; u->sec[ASM_SEC_BSS].used = 1; return 0;
            }
            if (rest_len >= 7 && memcmp(rest, ".rodata", 7) == 0) {
                c->cur = ASM_SEC_RODATA; u->sec[ASM_SEC_RODATA].used = 1; return 0;
            }
            if (rest_len >= 5 && memcmp(rest, ".text", 5) == 0) {
                c->cur = ASM_SEC_TEXT; u->sec[ASM_SEC_TEXT].used = 1; return 0;
            }
        }
    }

    if (n == 6 && memcmp(line, ".globl", 6) == 0) {
        if (rest_len == 0) { err(c, ".globl without symbol name"); return -1; }
        int si = sym_intern(c, rest, rest_len);
        u->syms[si].global = 1;
        return 0;
    }
    if (n == 7 && memcmp(line, ".balign", 7) == 0) {
        int64_t a;
        if (parse_int(rest, rest_len, &a) != 0 || a <= 0 || (a & (a - 1)) != 0) {
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
        // QBE only emits ".fill N,1,0"
        const char *ops[4];
        size_t lens[4];
        int cnt = split_ops(rest, rest_len, ops, lens);
        int64_t count, one, zero;
        if (cnt != 3 || parse_int(ops[0], lens[0], &count) != 0 || count < 0 ||
            parse_int(ops[1], lens[1], &one) != 0 || one != 1 ||
            parse_int(ops[2], lens[2], &zero) != 0 || zero != 0) {
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

// --- top-level driver -------------------------------------------------------------------

// Strip /* ... */ comments (QBE emits only single-line comments).
static void strip_comment(char *line) {
    char *p = strstr(line, "/*");
    if (!p) return;
    char *q = strstr(p, "*/");
    if (q) memmove(p, q + 2, strlen(q + 2) + 1);
    else *p = '\0';
}

static int resolve_fixups(Ctx *c) {
    for (size_t i = 0; i < c->nfixup; i++) {
        Fixup *f = &c->fixups[i];
        Label *lb = label_find(c, f->label, strlen(f->label));
        if (!lb || lb->section != ASM_SEC_TEXT) {
            c->line = f->line;
            err(c, "undefined branch target '%s'", f->label);
            return -1;
        }
        int64_t delta = (lb->offset - f->offset) / 4;
        uint8_t *ins = c->u->sec[ASM_SEC_TEXT].bytes + f->offset;
        uint32_t word = (uint32_t)ins[0] | ((uint32_t)ins[1] << 8) |
                        ((uint32_t)ins[2] << 16) | ((uint32_t)ins[3] << 24);
        if (f->cond) {
            if (delta < -(1 << 18) || delta >= (1 << 18)) {
                c->line = f->line;
                err(c, "conditional branch target out of range");
                return -1;
            }
            word |= ((uint32_t)delta & 0x7FFFFu) << 5;
        } else {
            if (delta < -(1 << 25) || delta >= (1 << 25)) {
                c->line = f->line;
                err(c, "branch target out of range");
                return -1;
            }
            word |= (uint32_t)delta & 0x03FFFFFFu;
        }
        ins[0] = (uint8_t)word;
        ins[1] = (uint8_t)(word >> 8);
        ins[2] = (uint8_t)(word >> 16);
        ins[3] = (uint8_t)(word >> 24);
    }
    return 0;
}

int asm_arm64_assemble(AsmUnit *u, const char *text, size_t len) {
    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.u = u;
    ctx.cur = ASM_SEC_TEXT;
    u->machine = EM_AARCH64;

    char *buf = malloc(len + 1);
    if (!buf) {
        snprintf(u->errmsg, sizeof(u->errmsg), "out of memory");
        u->errline = 0;
        u->has_error = 1;
        return 1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    int line_no = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && !ctx.failed;
         line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        ctx.line = line_no;
        strip_comment(line);
        size_t llen = strlen(line);
        const char *p = trim(line, &llen);
        if (llen == 0) continue;

        // label line: NAME: (QBE emits labels on their own line); must be
        // checked before directives because local labels start with '.'
        const char *colon = memchr(p, ':', llen);
        if (colon && colon == p + llen - 1) {
            size_t nlen = llen - 1;
            if (nlen == 0) { err(&ctx, "empty label"); break; }
            if (label_find(&ctx, p, nlen)) { err(&ctx, "duplicate label '%.*s'", (int)nlen, p); break; }
            label_add(&ctx, p, nlen, ctx.cur, (int64_t)u->sec[ctx.cur].len);
            if (!label_is_local(p, nlen)) {
                int si = sym_intern(&ctx, p, nlen);
                if (u->syms[si].section >= 0) {
                    err(&ctx, "duplicate symbol '%.*s'", (int)nlen, p);
                    break;
                }
                u->syms[si].section = ctx.cur;
                u->syms[si].value = (int64_t)u->sec[ctx.cur].len;
            }
            continue;
        }
        if (p[0] == '.') {
            assemble_directive(&ctx, p, llen);
            continue;
        }
        // instruction: mnemonic + operands
        size_t m = 0;
        while (m < llen && p[m] != ' ' && p[m] != '\t') m++;
        const char *mn = p;
        size_t mnlen = m;
        const char *opstr = p + m;
        size_t oplen = llen - m;
        opstr = trim(opstr, &oplen);
        const char *ops[5];
        size_t lens[5];
        int nops = 0;
        if (oplen > 0) {
            nops = split_ops(opstr, oplen, ops, lens);
            if (nops < 0) { err(&ctx, "malformed operand list"); break; }
        }
        assemble_insn(&ctx, mn, mnlen, ops, lens, nops);
    }

    if (!ctx.failed) resolve_fixups(&ctx);

    for (size_t i = 0; i < ctx.nlabel; i++) free(ctx.labels[i].name);
    free(ctx.labels);
    for (size_t i = 0; i < ctx.nfixup; i++) free(ctx.fixups[i].label);
    free(ctx.fixups);
    free(buf);
    return ctx.failed ? 1 : 0;
}
