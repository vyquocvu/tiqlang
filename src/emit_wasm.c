// M17.4.2: wasm32-wasi backend — lowers the Tiq SSA IR to a complete
// WebAssembly (MVP) binary module.
//
// Pipeline: IrModule -> in-memory wasm binary (.wasm) written to the output
// file. No external tools are involved.
//
// Design:
// - Each IR SSA register maps 1:1 to a wasm local. IR param registers are
//   allocated first by the lowerer, so wasm local index == IR register index
//   (params are the function's parameters; temps are extra locals).
// - Control flow is flattened into a `$pc` dispatch loop: every IR basic
//   block is emitted as one `if` case inside a single `loop`. Terminators
//   only update `$pc` (or `return`); the loop's back-edge re-dispatches, so
//   arbitrary CFGs need no structured-nesting analysis. Phi nodes are
//   materialized at branch edges: before a block branches to a successor,
//   each phi in the successor that this block feeds is written via local.set.
// - Strings live in the data segment (NUL-terminated), deduplicated and
//   offset-assigned before codegen. A small in-module runtime (strlen,
//   print_str, print_i64, print_bool, print_f64) serves print/len through the
//   wasi_snapshot_preview1 fd_write import; _start runs main then proc_exit.
// - wasm32 pointers are i32 (STR/STR_VIEW/REF/REF_MUT); ints map by size.
// - Unsupported IR constructs fail closed with a located diagnostic.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../include/emit_wasm.h"

// ---------------------------------------------------------------------------
// Byte buffer

typedef struct { uint8_t *d; size_t len, cap; } Buf;

static void b_init(Buf *b) { b->d = NULL; b->len = b->cap = 0; }
static void b_free(Buf *b) { free(b->d); b->d = NULL; b->len = b->cap = 0; }
static void b_need(Buf *b, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n) nc *= 2;
        void *tmp = realloc(b->d, nc);
        if (!tmp) { fprintf(stderr, "emit_wasm: out of memory\n"); exit(1); }
        b->d = tmp;
        b->cap = nc;
    }
}
static void b_byte(Buf *b, uint8_t v) { b_need(b, 1); b->d[b->len++] = v; }
static void b_bytes(Buf *b, const void *p, size_t n) {
    b_need(b, n);
    memcpy(b->d + b->len, p, n);
    b->len += n;
}

// LEB128 encoders.
static void w_uleb(Buf *b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        b_byte(b, byte);
    } while (v);
}
static void w_sleb(Buf *b, int64_t v) {
    uint8_t byte;
    for (;;) {
        byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
            b_byte(b, byte);
            return;
        }
        b_byte(b, byte | 0x80);
    }
}
static void w_name(Buf *b, const char *s, size_t n) {
    w_uleb(b, n);
    b_bytes(b, s, n);
}

// Section header wrapper.
static void sec(Buf *m, uint8_t id, const Buf *s) {
    b_byte(m, id);
    w_uleb(m, s->len);
    b_bytes(m, s->d, s->len);
}

// ---------------------------------------------------------------------------
// Valtypes

// 0x7F i32, 0x7E i64, 0x7D f32, 0x7C f64.
static uint8_t valtype(IrTypeKind k) {
    switch (k) {
        case IR_I64: case IR_U64: return 0x7E;
        case IR_F32: return 0x7D;
        case IR_F64: return 0x7C;
        default: return 0x7F; // bool, small ints, pointers (wasm32)
    }
}
static bool is_signed(IrTypeKind k) {
    switch (k) {
        case IR_U8: case IR_U16: case IR_U32: case IR_U64: return false;
        default: return true;
    }
}

// ---------------------------------------------------------------------------
// String pool + data layout
//
// String operands carry raw token text (surrounding quotes plus the
// LANGUAGE_SPEC §4 escapes \\ \" \n \r \t \0). decode_str() strips the quotes
// and decodes the escapes into the bytes stored in the data segment.

static int decode_str(const char *s, size_t len, Buf *out) {
    size_t i = 0, end = len;
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { i = 1; end = len - 1; }
    for (; i < end; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < end) {
            char e = s[i + 1];
            switch (e) {
                case '\\': b_byte(out, '\\'); break;
                case '"': b_byte(out, '"'); break;
                case 'n': b_byte(out, '\n'); break;
                case 'r': b_byte(out, '\r'); break;
                case 't': b_byte(out, '\t'); break;
                case '0': b_byte(out, '\0'); break;
                default: return -1; // lexer rejects others; fail closed
            }
            i++;
        } else {
            b_byte(out, c);
        }
    }
    return 0;
}

typedef struct { uint8_t *data; size_t len; uint32_t off; } StrEnt;
typedef struct { StrEnt *e; int count, cap; } StrPool;

static int pool_add_bytes(StrPool *p, const uint8_t *data, size_t n) {
    for (int i = 0; i < p->count; i++)
        if (p->e[i].len == n && memcmp(p->e[i].data, data, n) == 0) return i;
    if (p->count >= p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        void *tmp = realloc(p->e, p->cap * sizeof(StrEnt));
        if (!tmp) { fprintf(stderr, "emit_wasm: out of memory\n"); exit(1); }
        p->e = tmp;
    }
    uint8_t *copy = malloc(n ? n : 1);
    if (n) memcpy(copy, data, n);
    p->e[p->count] = (StrEnt){copy, n, 0};
    return p->count++;
}
static int pool_add_cstr(StrPool *p, const char *s) {
    return pool_add_bytes(p, (const uint8_t *)s, strlen(s));
}
static int pool_add_raw(StrPool *p, const char *s, size_t len) {
    Buf dec;
    b_init(&dec);
    if (decode_str(s, len, &dec) != 0) { b_free(&dec); return -1; }
    int id = pool_add_bytes(p, dec.d, dec.len);
    b_free(&dec);
    return id;
}
static int pool_off_raw(const StrPool *p, const char *s, size_t len) {
    Buf dec;
    b_init(&dec);
    if (decode_str(s, len, &dec) != 0) { b_free(&dec); return 0; }
    for (int i = 0; i < p->count; i++)
        if (p->e[i].len == dec.len && memcmp(p->e[i].data, dec.d, dec.len) == 0) {
            b_free(&dec);
            return (int)p->e[i].off;
        }
    b_free(&dec);
    return 0;
}
static void pool_free(StrPool *p) {
    for (int i = 0; i < p->count; i++) free(p->e[i].data);
    free(p->e);
    p->e = NULL; p->count = p->cap = 0;
}

// Collect the string literals (IR_CONST_STR operands) across the module.
static void collect_strings(const IrModule *mod, StrPool *pool) {
    for (int f = 0; f < mod->func_count; f++) {
        const IrFunction *fn = &mod->funcs[f];
        for (int i = 0; i < fn->instr_count; i++) {
            const IrInstr *ins = &fn->instrs[i];
            if (ins->op == IR_CONST_STR && ins->operand_count > 0 &&
                ins->operands[0].kind == IR_OP_STR)
                pool_add_raw(pool, ins->operands[0].str, ins->operands[0].len);
        }
    }
}

// ---------------------------------------------------------------------------
// Function type table (deduplicated)

#define MAX_PARAMS 16
typedef struct { uint8_t params[MAX_PARAMS]; int nparams; uint8_t result; int has_result; } Sig;
typedef struct { Sig *s; int count, cap; } TypeTable;

static int sig_add(TypeTable *t, const uint8_t *params, int nparams,
                   uint8_t result, int has_result) {
    if (nparams > MAX_PARAMS) return -1;
    for (int i = 0; i < t->count; i++) {
        if (t->s[i].nparams != nparams || t->s[i].has_result != has_result) continue;
        if (has_result && t->s[i].result != result) continue;
        if (memcmp(t->s[i].params, params, (size_t)nparams) != 0) continue;
        return i;
    }
    if (t->count >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        void *tmp = realloc(t->s, t->cap * sizeof(Sig));
        if (!tmp) { fprintf(stderr, "emit_wasm: out of memory\n"); exit(1); }
        t->s = tmp;
    }
    Sig *s = &t->s[t->count++];
    memset(s, 0, sizeof(*s));
    s->nparams = nparams;
    memcpy(s->params, params, (size_t)nparams);
    s->result = result;
    s->has_result = has_result;
    return t->count - 1;
}

// ---------------------------------------------------------------------------
// Function table (name -> user function), used to resolve IR_CALL targets.

typedef struct { const char *name; size_t len; const IrFunction *fn; int sig_idx; } FuncEnt;
typedef struct { FuncEnt *e; int count, cap; } FuncTable;

static int ft_add(FuncTable *ft, const IrFunction *fn, int sig_idx) {
    if (ft->count >= ft->cap) {
        ft->cap = ft->cap ? ft->cap * 2 : 8;
        void *tmp = realloc(ft->e, ft->cap * sizeof(FuncEnt));
        if (!tmp) { fprintf(stderr, "emit_wasm: out of memory\n"); exit(1); }
        ft->e = tmp;
    }
    ft->e[ft->count] = (FuncEnt){fn->name, fn->name_len, fn, sig_idx};
    return ft->count++;
}
static int ft_lookup(const FuncTable *ft, const char *name, size_t len) {
    for (int i = 0; i < ft->count; i++)
        if (ft->e[i].len == len && memcmp(ft->e[i].name, name, len) == 0) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Data layout constants. d_buf is the runtime decimal buffer, d_iov the
// WASI iovec {ptr, len}, d_nw the nwritten slot; strings follow.

enum { BUF_SIZE = 48 };

typedef struct {
    uint32_t buf, iov, nw;
    uint32_t off_true, off_false;
    uint32_t off_nan, off_inf, off_ninf, off_m0, off_0;
    uint32_t off_nl;
} RtCtx;

// Function index layout.
// FUNC_ALLOC: internal bump-allocator (i32 size -> i32 ptr) used by
// IR_STRUCT_INIT and IR_ARRAY_INIT to allocate struct/array memory.
enum {
    FUNC_FD_WRITE = 0,
    FUNC_PROC_EXIT = 1,
    FUNC_STRLEN = 2,
    FUNC_PRINT_STR = 3,
    FUNC_PRINT_I64 = 4,
    FUNC_PRINT_BOOL = 5,
    FUNC_PRINT_F64 = 6,
    FUNC_PRINT_NL = 7,
    FUNC_ALLOC = 8,     // bump allocator: (size: i32) -> i32 ptr
    FUNC_USER_BASE = 9,
};

// ---------------------------------------------------------------------------
// Register type map (register -> IrTypeKind), mirroring emit_qbe.c.

#define MAX_REGS 4096
typedef struct { IrTypeKind kinds[MAX_REGS]; int count; } RegTypes;

static void regtypes_build(const IrFunction *func, RegTypes *rt) {
    memset(rt, 0, sizeof(*rt));
    rt->count = 0;
    for (int p = 0; p < func->param_count; p++) {
        rt->kinds[p] = func->param_types[p].kind;
        if (p + 1 > rt->count) rt->count = p + 1;
    }
    for (int b = 0; b < func->block_count; b++) {
        const IrBlock *blk = &func->blocks[b];
        for (int p = 0; p < blk->phi_count; p++) {
            int r = blk->phis[p].dst;
            if (r >= 0 && r < MAX_REGS) {
                rt->kinds[r] = blk->phis[p].type.kind;
                if (r >= rt->count) rt->count = r + 1;
            }
        }
        for (int i = blk->instr_start; i < blk->instr_end; i++) {
            const IrInstr *ins = &func->instrs[i];
            if (ins->dst >= 0 && ins->dst < MAX_REGS) {
                rt->kinds[ins->dst] = ins->dst_type.kind;
                if (ins->dst >= rt->count) rt->count = ins->dst + 1;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Small bytecode writers for the in-module runtime and for IR emission.

static void i32c(Buf *o, int32_t v) { b_byte(o, 0x41); w_sleb(o, v); }
static void f64b(Buf *o, uint64_t bits) {
    b_byte(o, 0x44);
    for (int i = 0; i < 8; i++) b_byte(o, (uint8_t)(bits >> (8 * i)));
}
static void f64c(Buf *o, double v) { uint64_t b; memcpy(&b, &v, 8); f64b(o, b); }
static void lg(Buf *o, uint32_t i) { b_byte(o, 0x20); w_uleb(o, i); }   // local.get
static void ls(Buf *o, uint32_t i) { b_byte(o, 0x21); w_uleb(o, i); }   // local.set
static void op(Buf *o, uint8_t b) { b_byte(o, b); }
static void call(Buf *o, uint32_t f) { b_byte(o, 0x10); w_uleb(o, f); }
static void blk(Buf *o) { b_byte(o, 0x02); b_byte(o, 0x40); }           // block void
static void lop(Buf *o) { b_byte(o, 0x03); b_byte(o, 0x40); }           // loop void
static void iff(Buf *o) { b_byte(o, 0x04); b_byte(o, 0x40); }           // if void
static void els(Buf *o) { b_byte(o, 0x05); }
static void end(Buf *o) { b_byte(o, 0x0B); }
static void br0(Buf *o) { b_byte(o, 0x0C); b_byte(o, 0x00); } // br 0: loop back-edge
static void ret(Buf *o) { b_byte(o, 0x0F); }
static void brf(Buf *o, uint32_t d) { b_byte(o, 0x0D); w_uleb(o, d); }
static void ld8(Buf *o) { b_byte(o, 0x2D); b_byte(o, 0x00); b_byte(o, 0x00); } // i32.load8_u
static void st32(Buf *o) { b_byte(o, 0x36); b_byte(o, 0x02); b_byte(o, 0x00); }
static void st8(Buf *o) { b_byte(o, 0x3A); b_byte(o, 0x00); b_byte(o, 0x00); }

// --- Runtime helper bodies --------------------------------------------------

// strlen(ptr: i32) -> i32; locals: [1] = i
static void emit_strlen(Buf *o) {
    b_byte(o, 0x01); b_byte(o, 0x01); b_byte(o, 0x7F);
    blk(o); lop(o);
    lg(o, 0); lg(o, 1); op(o, 0x6A); ld8(o); op(o, 0x45); brf(o, 1);
    lg(o, 1); i32c(o, 1); op(o, 0x6A); ls(o, 1);
    br0(o);
    end(o); end(o);
    lg(o, 1);
    end(o);
}

// print_str(ptr: i32) -> (); locals: [1] = n
static void emit_print_str(Buf *o, uint32_t iov, uint32_t nw, uint32_t off_nl) {
    b_byte(o, 0x01); b_byte(o, 0x01); b_byte(o, 0x7F);
    lg(o, 0); call(o, FUNC_STRLEN); ls(o, 1);
    i32c(o, (int32_t)iov); lg(o, 0); st32(o);
    i32c(o, (int32_t)(iov + 4)); lg(o, 1); st32(o);
    i32c(o, 1); i32c(o, (int32_t)iov); i32c(o, 1); i32c(o, (int32_t)nw);
    call(o, FUNC_FD_WRITE); op(o, 0x1A); // drop
    i32c(o, (int32_t)off_nl); call(o, FUNC_PRINT_NL); // trailing newline
    end(o);
}

// print_nl(off: i32) -> (); writes a single newline byte via fd_write.
// Reuses the shared iov/nw slots. Called by the print helpers so every
// print emits a trailing newline, per LANGUAGE_SPEC §12.
static void emit_print_nl(Buf *o, uint32_t iov, uint32_t nw) {
    b_byte(o, 0x00);
    i32c(o, (int32_t)iov); lg(o, 0); st32(o);       // iov.ptr = off
    i32c(o, (int32_t)(iov + 4)); i32c(o, 1); st32(o); // iov.len = 1
    i32c(o, 1); i32c(o, (int32_t)iov); i32c(o, 1); i32c(o, (int32_t)nw);
    call(o, FUNC_FD_WRITE); op(o, 0x1A); // drop
    end(o);
}

// print_i64(n: i64) -> (); locals: [1]=ptr [2]=digits [3]=is_neg
static void emit_print_i64(Buf *o, uint32_t buf_off, uint32_t iov, uint32_t nw, uint32_t off_nl) {
    b_byte(o, 0x01); b_byte(o, 0x03); b_byte(o, 0x7F);
    // is_neg = n < 0
    lg(o, 0); b_byte(o, 0x42); w_sleb(o, 0); op(o, 0x53); ls(o, 3);
    // if (is_neg) n = -n
    lg(o, 3); iff(o);
    b_byte(o, 0x42); w_sleb(o, 0); lg(o, 0); op(o, 0x7D); ls(o, 0);
    end(o);
    // ptr = buf + size ; digits = 0
    i32c(o, (int32_t)buf_off); i32c(o, BUF_SIZE); op(o, 0x6A); ls(o, 1);
    i32c(o, 0); ls(o, 2);
    // do { ptr--; *ptr = wrap(n%10)+'0'; n/=10; digits++ } while (n != 0)
    blk(o); lop(o);
    lg(o, 1); i32c(o, 1); op(o, 0x6B); ls(o, 1);
    lg(o, 1);
    lg(o, 0); b_byte(o, 0x42); w_sleb(o, 10); op(o, 0x81); op(o, 0xA7);
    i32c(o, 48); op(o, 0x6A); st8(o);
    lg(o, 0); b_byte(o, 0x42); w_sleb(o, 10); op(o, 0x7F); ls(o, 0);
    lg(o, 2); i32c(o, 1); op(o, 0x6A); ls(o, 2);
    lg(o, 0); b_byte(o, 0x42); w_sleb(o, 0); op(o, 0x51); brf(o, 1);
    br0(o);
    end(o); end(o);
    // if (is_neg) { ptr--; *ptr = '-'; digits++ }
    lg(o, 3); iff(o);
    lg(o, 1); i32c(o, 1); op(o, 0x6B); ls(o, 1);
    lg(o, 1); i32c(o, 45); st8(o);
    lg(o, 2); i32c(o, 1); op(o, 0x6A); ls(o, 2);
    end(o);
    // fd_write(1, iov{ptr,digits}, 1, &nw)
    i32c(o, (int32_t)iov); lg(o, 1); st32(o);
    i32c(o, (int32_t)(iov + 4)); lg(o, 2); st32(o);
    i32c(o, 1); i32c(o, (int32_t)iov); i32c(o, 1); i32c(o, (int32_t)nw);
    call(o, FUNC_FD_WRITE); op(o, 0x1A);
    i32c(o, (int32_t)off_nl); call(o, FUNC_PRINT_NL); // trailing newline
    end(o);
}

// print_bool(b: i32) -> ()
static void emit_print_bool(Buf *o, uint32_t off_true, uint32_t off_false) {
    b_byte(o, 0x00);
    lg(o, 0); iff(o);
    i32c(o, (int32_t)off_true); call(o, FUNC_PRINT_STR);
    els(o);
    i32c(o, (int32_t)off_false); call(o, FUNC_PRINT_STR);
    end(o);
    end(o);
}

// Trim trailing '0's after the decimal point, then a dangling '.'.
// o=[3] out pointer, n=[4] out length (locals of print_f64).
static void emit_trim(Buf *o) {
    blk(o); lop(o);
    lg(o, 4); i32c(o, 2); op(o, 0x4E);
    lg(o, 3); i32c(o, 1); op(o, 0x6B); ld8(o); i32c(o, 48); op(o, 0x46);
    op(o, 0x71); op(o, 0x45); brf(o, 1);
    lg(o, 3); i32c(o, 1); op(o, 0x6B); ls(o, 3);
    lg(o, 4); i32c(o, 1); op(o, 0x6B); ls(o, 4);
    br0(o);
    end(o); end(o);
    lg(o, 4); i32c(o, 2); op(o, 0x4E);
    lg(o, 3); i32c(o, 1); op(o, 0x6B); ld8(o); i32c(o, 46); op(o, 0x46);
    op(o, 0x71); iff(o);
    lg(o, 3); i32c(o, 1); op(o, 0x6B); ls(o, 3);
    lg(o, 4); i32c(o, 1); op(o, 0x6B); ls(o, 4);
    end(o);
}

// print_f64(x: f64) -> (); 6 significant digits, %g-style output.
// locals: [1]=is_neg [2]=e [3]=o [4]=n [5]=d [6]=i [7]=k [8]=carry (i32);
//         [9]=pow [10]=r (f64)
static void emit_print_f64(Buf *o, const RtCtx *rc) {
    b_byte(o, 0x02); b_byte(o, 0x08); b_byte(o, 0x7F);
    b_byte(o, 0x02); b_byte(o, 0x7C);
    // is_neg = x < 0
    lg(o, 0); f64c(o, 0.0); op(o, 0x63); ls(o, 1);
    // if (x != x) { print "nan"; return }
    lg(o, 0); lg(o, 0); op(o, 0x62); iff(o);
    i32c(o, (int32_t)rc->off_nan); call(o, FUNC_PRINT_STR); ret(o);
    end(o);
    // if (is_neg) x = -x
    lg(o, 1); iff(o); lg(o, 0); op(o, 0x9A); ls(o, 0); end(o);
    // if (x == +inf) { print sign+"inf"; return }
    lg(o, 0); f64b(o, 0x7FF0000000000000ULL); op(o, 0x61); iff(o);
    lg(o, 1); iff(o);
    i32c(o, (int32_t)rc->off_ninf); call(o, FUNC_PRINT_STR);
    els(o);
    i32c(o, (int32_t)rc->off_inf); call(o, FUNC_PRINT_STR);
    end(o); ret(o);
    end(o);
    // if (x == 0) { print sign+"0"; return }
    lg(o, 0); f64c(o, 0.0); op(o, 0x61); iff(o);
    lg(o, 1); iff(o);
    i32c(o, (int32_t)rc->off_m0); call(o, FUNC_PRINT_STR);
    els(o);
    i32c(o, (int32_t)rc->off_0); call(o, FUNC_PRINT_STR);
    end(o); ret(o);
    end(o);
    // e = 0 ; pow = 1.0 ; while pow*10 <= x { pow*=10; e++ }
    i32c(o, 0); ls(o, 2);
    f64c(o, 1.0); ls(o, 9);
    blk(o); lop(o);
    lg(o, 9); f64c(o, 10.0); op(o, 0xA2); lg(o, 0); op(o, 0x65); op(o, 0x45); brf(o, 1);
    lg(o, 9); f64c(o, 10.0); op(o, 0xA2); ls(o, 9);
    lg(o, 2); i32c(o, 1); op(o, 0x6A); ls(o, 2);
    br0(o);
    end(o); end(o);
    // while pow > x { pow /= 10; e-- }
    blk(o); lop(o);
    lg(o, 9); lg(o, 0); op(o, 0x64); op(o, 0x45); brf(o, 1);
    lg(o, 9); f64c(o, 10.0); op(o, 0xA3); ls(o, 9);
    lg(o, 2); i32c(o, 1); op(o, 0x6B); ls(o, 2);
    br0(o);
    end(o); end(o);
    // r = x / pow ; if (r >= 10) r = 9.999999999
    lg(o, 0); lg(o, 9); op(o, 0xA3); ls(o, 10);
    lg(o, 10); f64c(o, 10.0); op(o, 0x66); iff(o); // if (r >= 10.0)
    f64c(o, 9.999999999); ls(o, 10);
    end(o);
    // o = buf + 8 ; n = 0 ; if (is_neg) { *o='-'; o++; n++ }
    i32c(o, (int32_t)(rc->buf + 8)); ls(o, 3);
    i32c(o, 0); ls(o, 4);
    lg(o, 1); iff(o);
    lg(o, 3); i32c(o, 45); st8(o);
    lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
    lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
    end(o);
    // extract 7 significant digits (7th used for rounding)
    i32c(o, 0); ls(o, 6);
    i32c(o, 0); ls(o, 8); // carry = 0
    blk(o); lop(o);
    lg(o, 6); i32c(o, 7); op(o, 0x4E); brf(o, 1);
    lg(o, 10); f64c(o, 1e-9); op(o, 0xA0); op(o, 0xAA); ls(o, 5); // d = trunc(r + eps)
    lg(o, 5); i32c(o, 10); op(o, 0x4E); iff(o); // if (d >= 10)
    i32c(o, 1); ls(o, 5); // d = 1
    lg(o, 2); i32c(o, 1); op(o, 0x6A); ls(o, 2); // e++
    i32c(o, 1); ls(o, 8); // carry = 1
    end(o);
    i32c(o, (int32_t)rc->buf); lg(o, 6); op(o, 0x6A); lg(o, 5); i32c(o, 48); op(o, 0x6A); st8(o);
    lg(o, 10); lg(o, 5); op(o, 0xB7); op(o, 0xA1); f64c(o, 10.0); op(o, 0xA2); ls(o, 10);
    lg(o, 6); i32c(o, 1); op(o, 0x6A); ls(o, 6);
    br0(o);
    end(o); end(o); // end loop, end block
    // round: if buf[6] >= '5', carry into buf[5]
    i32c(o, (int32_t)rc->buf); i32c(o, 6); op(o, 0x6A); ld8(o);
    i32c(o, 53); op(o, 0x4E); iff(o); // if (buf[6] >= '5')
    i32c(o, 1); ls(o, 8); // carry = 1
    i32c(o, 5); ls(o, 7); // j = 5 (6th digit, 0-indexed)
    blk(o); lop(o);
    lg(o, 7); i32c(o, 0); op(o, 0x48); brf(o, 1); // if (j >= 0)
    i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o);
    i32c(o, 1); op(o, 0x6A); // buf[j] + 1 (ASCII increment)
    i32c(o, 58); op(o, 0x4E); iff(o); // if >= '9'+1 (i.e., > '9')
    i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); i32c(o, 48); st8(o); // buf[j] = '0'
    lg(o, 7); i32c(o, 1); op(o, 0x6B); ls(o, 7); // j--
    br0(o); // continue
    els(o);
    // buf[j] = buf[j] + 1
    // store8 needs [address, value] on stack (value on top).
    // Compute address first, then value.
    i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); // address = buf + j
    i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o); i32c(o, 1); op(o, 0x6A); // value = buf[j] + 1
    st8(o); // store value at address
    i32c(o, 0); ls(o, 8); // carry = 0 (done)
    i32c(o, -1); ls(o, 7); // j = -1 (exit loop on next check)
    end(o);
    br0(o); // continue loop (will exit since j=0 < 0 is false → brf exits)
    end(o); end(o); // end if/loop/block
    // if carry still set, buf[0] overflowed
    lg(o, 8); iff(o);
    i32c(o, (int32_t)rc->buf); i32c(o, 49); st8(o); // buf[0] = '1'
    i32c(o, 1); ls(o, 7);
    blk(o); lop(o);
    lg(o, 7); i32c(o, 6); op(o, 0x4E); brf(o, 1);
    i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); i32c(o, 48); st8(o);
    lg(o, 7); i32c(o, 1); op(o, 0x6A); ls(o, 7);
    br0(o);
    end(o); end(o);
    lg(o, 2); i32c(o, 1); op(o, 0x6A); ls(o, 2); // e++
    end(o);
    end(o); // end if(buf[6] >= '5')
    // i = (e >= 6 || e < -4)  -> scientific?
    lg(o, 2); i32c(o, 6); op(o, 0x4E);
    lg(o, 2); i32c(o, -4); op(o, 0x48);
    op(o, 0x72); ls(o, 6);
    lg(o, 6); iff(o);
        // scientific: d0 '.' d1..d5 (trimmed), then e<sign><2+digits>
        lg(o, 3); i32c(o, (int32_t)rc->buf); ld8(o); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        lg(o, 3); i32c(o, 46); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        i32c(o, 1); ls(o, 7);
        blk(o); lop(o);
        lg(o, 7); i32c(o, 6); op(o, 0x4E); brf(o, 1);
        lg(o, 3); i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        lg(o, 7); i32c(o, 1); op(o, 0x6A); ls(o, 7);
        br0(o);
        end(o); end(o);
        emit_trim(o);
        lg(o, 3); i32c(o, 101); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        // sign + |e|
        lg(o, 2); i32c(o, 0); op(o, 0x4E); iff(o);
        lg(o, 3); i32c(o, 43); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        lg(o, 2); ls(o, 7);
        els(o);
        lg(o, 3); i32c(o, 45); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        i32c(o, 0); lg(o, 2); op(o, 0x6B); ls(o, 7);
        end(o);
        // exponent digits (reversed into buf)
        i32c(o, 0); ls(o, 6);
        blk(o); lop(o);
        i32c(o, (int32_t)rc->buf); lg(o, 6); op(o, 0x6A);
        lg(o, 7); i32c(o, 10); op(o, 0x6F); i32c(o, 48); op(o, 0x6A); st8(o);
        lg(o, 7); i32c(o, 10); op(o, 0x6D); ls(o, 7);
        lg(o, 6); i32c(o, 1); op(o, 0x6A); ls(o, 6);
        lg(o, 7); op(o, 0x45); brf(o, 1);
        br0(o);
        end(o); end(o);
        // pad to at least 2 digits
        lg(o, 6); i32c(o, 2); op(o, 0x48); iff(o);
        lg(o, 3); i32c(o, 48); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        end(o);
        // emit reversed exponent digits
        lg(o, 6); i32c(o, 1); op(o, 0x6B); ls(o, 7);
        blk(o); lop(o);
        lg(o, 7); i32c(o, 0); op(o, 0x48); brf(o, 1);
        lg(o, 3); i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o); st8(o);
        lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
        lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
        lg(o, 7); i32c(o, 1); op(o, 0x6B); ls(o, 7);
        br0(o);
        end(o); end(o);
    els(o);
        // fixed
        lg(o, 2); i32c(o, 0); op(o, 0x4E); iff(o); // if (e >= 0)
            // integer digits buf[0..e]
            i32c(o, 0); ls(o, 7);
            blk(o); lop(o);
            lg(o, 7); lg(o, 2); op(o, 0x4A); brf(o, 1);
            lg(o, 3); i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o); st8(o);
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            lg(o, 7); i32c(o, 1); op(o, 0x6A); ls(o, 7);
            br0(o);
            end(o); end(o);
            // fractional digits buf[e+1..5] (only when e < 6)
            lg(o, 2); i32c(o, 5); op(o, 0x48); iff(o); // if (e < 5) — has fractional part
            lg(o, 2); i32c(o, 1); op(o, 0x6A); ls(o, 7); // i = e + 1
            blk(o); lop(o);
            lg(o, 7); i32c(o, 6); op(o, 0x4E); brf(o, 1);
            lg(o, 7); lg(o, 2); i32c(o, 1); op(o, 0x6A); op(o, 0x46); iff(o); // if (i == e+1)
            lg(o, 3); i32c(o, 46); st8(o); // '.'
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            end(o);
            lg(o, 3); i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o); st8(o);
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            lg(o, 7); i32c(o, 1); op(o, 0x6A); ls(o, 7);
            br0(o);
            end(o); end(o);
            emit_trim(o);
            end(o); // end if (e < 5)
        els(o);
            // e < 0: "0." then (-e-1) zeros then digits
            lg(o, 3); i32c(o, 48); st8(o);
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            lg(o, 3); i32c(o, 46); st8(o);
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            i32c(o, 0); ls(o, 7);
            blk(o); lop(o);
            lg(o, 7); i32c(o, 0); lg(o, 2); op(o, 0x6B); i32c(o, 1); op(o, 0x6B); op(o, 0x4E); brf(o, 1);
            lg(o, 3); i32c(o, 48); st8(o);
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            lg(o, 7); i32c(o, 1); op(o, 0x6A); ls(o, 7);
            br0(o);
            end(o); end(o);
            i32c(o, 0); ls(o, 7);
            blk(o); lop(o);
            lg(o, 7); i32c(o, 6); op(o, 0x4E); brf(o, 1);
            lg(o, 3); i32c(o, (int32_t)rc->buf); lg(o, 7); op(o, 0x6A); ld8(o); st8(o);
            lg(o, 3); i32c(o, 1); op(o, 0x6A); ls(o, 3);
            lg(o, 4); i32c(o, 1); op(o, 0x6A); ls(o, 4);
            lg(o, 7); i32c(o, 1); op(o, 0x6A); ls(o, 7);
            br0(o);
            end(o); end(o);
            emit_trim(o);
        end(o);
    end(o);
    // fd_write(1, iov{buf+8, n}, 1, &nw)
    i32c(o, (int32_t)rc->iov); i32c(o, (int32_t)(rc->buf + 8)); st32(o);
    i32c(o, (int32_t)(rc->iov + 4)); lg(o, 4); st32(o);
    i32c(o, 1); i32c(o, (int32_t)rc->iov); i32c(o, 1); i32c(o, (int32_t)rc->nw);
    call(o, FUNC_FD_WRITE); op(o, 0x1A);
    i32c(o, (int32_t)rc->off_nl); call(o, FUNC_PRINT_NL); // trailing newline
    end(o);
}

// store/load helpers for 32-bit and 64-bit linear memory access.
static void st64(Buf *o) { b_byte(o, 0x37); b_byte(o, 0x03); b_byte(o, 0x00); } // i64.store align=8
static void ld64(Buf *o) { b_byte(o, 0x29); b_byte(o, 0x03); b_byte(o, 0x00); } // i64.load align=8

// alloc(size: i32) -> i32: bump allocator from global 0 (__heap_ptr).
// 8-byte aligned. No free. Each call returns a fresh block of `size` bytes.
// locals: none
static void emit_alloc(Buf *o) {
    b_byte(o, 0x00); // no extra locals
    // 1. push old heap_ptr as return value
    b_byte(o, 0x23); w_uleb(o, 0); // global.get 0
    // 2. compute new heap_ptr = old heap_ptr + ((size + 7) & ~7)
    b_byte(o, 0x23); w_uleb(o, 0); // global.get 0
    lg(o, 0);                       // size arg
    i32c(o, 7); op(o, 0x6A);        // size + 7
    i32c(o, -8); op(o, 0x71);       // & ~7
    op(o, 0x6A);                    // old_heap_ptr + aligned_size
    b_byte(o, 0x24); w_uleb(o, 0); // global.set 0 (updates heap_ptr)
    // stack has exactly 1 element: old heap_ptr (the return value)
    end(o);
}

// ---------------------------------------------------------------------------
// Operand / value emission (pushes a value of `vt` on the stack)

typedef struct {
    Buf *out;
    const IrFunction *func;
    const RegTypes *rt;
    const StrPool *pool;
    const FuncTable *ft;
    int param_count;
    int pc_reg;
} CodeCtx;

static int emit_value(CodeCtx *cc, const IrOperand *op, uint8_t vt,
                      char *err, size_t errlen) {
    Buf *o = cc->out;
    switch (op->kind) {
        case IR_OP_REG:
            b_byte(o, 0x20);
            w_uleb(o, (uint64_t)op->reg);
            return 0;
        case IR_OP_IMM:
            if (vt == 0x7E) { b_byte(o, 0x42); w_sleb(o, op->imm); return 0; }
            if (vt == 0x7D || vt == 0x7C) {
                snprintf(err, errlen, "wasm: unsupported float immediate operand");
                return -1;
            }
            b_byte(o, 0x41);
            w_sleb(o, op->imm);
            return 0;
        default:
            snprintf(err, errlen, "wasm: unsupported operand kind %d", (int)op->kind);
            return -1;
    }
}

// Emit one IR instruction (inside a block's dispatch case).
static int emit_instr(CodeCtx *cc, const IrInstr *ins, char *err, size_t errlen) {
    Buf *o = cc->out;
    IrTypeKind tk = ins->dst_type.kind;
    uint8_t vt = valtype(tk);
    int dst = ins->dst;
    const IrOperand *a0 = ins->operand_count > 0 ? &ins->operands[0] : NULL;
    const IrOperand *a1 = ins->operand_count > 1 ? &ins->operands[1] : NULL;

    switch (ins->op) {
        case IR_CONST_INT:
        case IR_CONST_BOOL:
            if (a0) {
                if (emit_value(cc, a0, vt, err, errlen) != 0) return -1;
                b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            }
            return 0;

        case IR_CONST_FLOAT: {
            double dv;
            long long bits = a0 ? a0->imm : 0;
            memcpy(&dv, &bits, sizeof(double));
            if (vt == 0x7C) {
                b_byte(o, 0x44);
                b_bytes(o, &dv, 8);
            } else {
                float fv = (float)dv;
                b_byte(o, 0x43);
                b_bytes(o, &fv, 4);
            }
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_CONST_STR: {
            if (a0 && a0->kind == IR_OP_STR) {
                int off = pool_off_raw(cc->pool, a0->str, a0->len);
                b_byte(o, 0x41);
                w_sleb(o, off);
                b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            }
            return 0;
        }

        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_DIV: case IR_MOD: {
            if (!a0 || !a1) return 0;
            if (emit_value(cc, a0, vt, err, errlen) != 0) return -1;
            if (emit_value(cc, a1, vt, err, errlen) != 0) return -1;
            uint8_t opb;
            switch (ins->op) {
                case IR_ADD:
                    opb = vt == 0x7E ? 0x7C : vt == 0x7D ? 0x92 : vt == 0x7C ? 0xA0 : 0x6A;
                    break;
                case IR_SUB:
                    opb = vt == 0x7E ? 0x7D : vt == 0x7D ? 0x93 : vt == 0x7C ? 0xA1 : 0x6B;
                    break;
                case IR_MUL:
                    opb = vt == 0x7E ? 0x7E : vt == 0x7D ? 0x94 : vt == 0x7C ? 0xA2 : 0x6C;
                    break;
                case IR_DIV:
                    if (vt == 0x7C) opb = 0xA3;
                    else if (vt == 0x7D) opb = 0x95;
                    else opb = vt == 0x7E ? (is_signed(tk) ? 0x7F : 0x80)
                                          : (is_signed(tk) ? 0x6D : 0x6E);
                    break;
                default: // IR_MOD
                    if (vt == 0x7C || vt == 0x7D) {
                        snprintf(err, errlen, "wasm: fmod not supported");
                        return -1;
                    }
                    opb = vt == 0x7E ? (is_signed(tk) ? 0x81 : 0x82)
                                     : (is_signed(tk) ? 0x6F : 0x70);
                    break;
            }
            b_byte(o, opb);
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_NEG: {
            if (!a0) return 0;
            if (vt == 0x7C) {
                if (emit_value(cc, a0, vt, err, errlen) != 0) return -1;
                b_byte(o, 0x9A); // f64.neg
            } else if (vt == 0x7D) {
                if (emit_value(cc, a0, vt, err, errlen) != 0) return -1;
                b_byte(o, 0x8C); // f32.neg
            } else {
                // 0 - x
                if (vt == 0x7E) { b_byte(o, 0x42); w_sleb(o, 0); }
                else { b_byte(o, 0x41); w_sleb(o, 0); }
                if (emit_value(cc, a0, vt, err, errlen) != 0) return -1;
                b_byte(o, vt == 0x7E ? 0x7D : 0x6B);
            }
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT: case IR_CMP_LE:
        case IR_CMP_GT: case IR_CMP_GE: {
            if (!a0 || !a1) return 0;
            // Operand type comes from either register operand; the result is bool.
            IrTypeKind otk = IR_I64;
            if (a0->kind == IR_OP_REG && a0->reg < cc->rt->count) otk = cc->rt->kinds[a0->reg];
            else if (a1->kind == IR_OP_REG && a1->reg < cc->rt->count) otk = cc->rt->kinds[a1->reg];
            uint8_t ovt = valtype(otk);
            if (emit_value(cc, a0, ovt, err, errlen) != 0) return -1;
            if (emit_value(cc, a1, ovt, err, errlen) != 0) return -1;
            int isf = ovt == 0x7D || ovt == 0x7C;
            int uns = !is_signed(otk);
            static const uint8_t i32ops[6] = {0x46, 0x47, 0x48, 0x4C, 0x4A, 0x4E};
            static const uint8_t i32uops[6] = {0x46, 0x47, 0x49, 0x4D, 0x4B, 0x4F};
            static const uint8_t i64ops[6] = {0x51, 0x52, 0x53, 0x57, 0x55, 0x59};
            static const uint8_t i64uops[6] = {0x51, 0x52, 0x54, 0x58, 0x56, 0x5A};
            static const uint8_t f32ops[6] = {0x5B, 0x5C, 0x5D, 0x5F, 0x5E, 0x60};
            static const uint8_t f64ops[6] = {0x61, 0x62, 0x63, 0x65, 0x64, 0x66};
            int idx = (int)(ins->op - IR_CMP_EQ);
            if (isf) {
                b_byte(o, ovt == 0x7D ? f32ops[idx] : f64ops[idx]);
            } else if (ovt == 0x7E) {
                b_byte(o, uns ? i64uops[idx] : i64ops[idx]);
            } else {
                b_byte(o, uns ? i32uops[idx] : i32ops[idx]);
            }
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_AND: case IR_OR: case IR_BIT_AND: case IR_BIT_OR: case IR_BIT_XOR: {
            if (!a0 || !a1) return 0;
            uint8_t vt2 = (ins->op == IR_AND || ins->op == IR_OR) ? 0x7F : valtype(tk);
            if (emit_value(cc, a0, vt2, err, errlen) != 0) return -1;
            if (emit_value(cc, a1, vt2, err, errlen) != 0) return -1;
            uint8_t opb;
            switch (ins->op) {
                case IR_AND: case IR_BIT_AND: opb = vt2 == 0x7E ? 0x83 : 0x71; break;
                case IR_OR: case IR_BIT_OR:   opb = vt2 == 0x7E ? 0x84 : 0x72; break;
                default:                       opb = vt2 == 0x7E ? 0x85 : 0x73; break;
            }
            b_byte(o, opb);
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_NOT: {
            if (!a0) return 0;
            if (emit_value(cc, a0, 0x7F, err, errlen) != 0) return -1;
            b_byte(o, 0x41); w_sleb(o, 1);
            b_byte(o, 0x73); // i32.xor (0 ^ x -> 1 ^ x = !x)
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_BIT_SHL: case IR_BIT_SHR: {
            if (!a0 || !a1) return 0;
            uint8_t vt2 = valtype(tk);
            if (emit_value(cc, a0, vt2, err, errlen) != 0) return -1;
            if (emit_value(cc, a1, vt2, err, errlen) != 0) return -1;
            uint8_t opb;
            if (vt2 == 0x7E) {
                opb = ins->op == IR_BIT_SHL ? 0x86
                    : (is_signed(tk) ? 0x87 : 0x88);
            } else {
                opb = ins->op == IR_BIT_SHL ? 0x74
                    : (is_signed(tk) ? 0x75 : 0x76);
            }
            b_byte(o, opb);
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_RET: {
            if (ins->operand_count >= 1) {
                uint8_t rvt = valtype(cc->func->return_type.kind);
                if (emit_value(cc, &ins->operands[0], rvt, err, errlen) != 0) return -1;
            }
            b_byte(o, 0x0F); // return
            return 0;
        }

        case IR_CALL: {
            if (ins->operand_count < 1 || ins->operands[0].kind != IR_OP_STR) return 0;
            const char *nm = ins->operands[0].str;
            size_t nl = ins->operands[0].len;
            int slot = ft_lookup(cc->ft, nm, nl);
            if (slot < 0) {
                snprintf(err, errlen, "wasm: call to unknown function '%.*s'", (int)nl, nm);
                return -1;
            }
            const IrFunction *callee = cc->ft->e[slot].fn;
            for (int a = 1; a < ins->operand_count; a++) {
                uint8_t pt = (a - 1 < callee->param_count)
                    ? valtype(callee->param_types[a - 1].kind) : 0x7F;
                if (emit_value(cc, &ins->operands[a], pt, err, errlen) != 0) return -1;
            }
            b_byte(o, 0x10);
            w_uleb(o, (uint64_t)(FUNC_USER_BASE + slot));
            if (dst >= 0) { b_byte(o, 0x21); w_uleb(o, (uint64_t)dst); }
            return 0;
        }

        case IR_PRINT: {
            if (ins->operand_count < 1) return 0;
            const IrOperand *arg = &ins->operands[0];
            IrTypeKind ak = IR_I64;
            if (arg->kind == IR_OP_REG && arg->reg < cc->rt->count) ak = cc->rt->kinds[arg->reg];
            if (arg->kind == IR_OP_STR) {
                int off = pool_off_raw(cc->pool, arg->str, arg->len);
                b_byte(o, 0x41);
                w_sleb(o, off);
                b_byte(o, 0x10); w_uleb(o, FUNC_PRINT_STR);
            } else if (ak == IR_STR || ak == IR_STR_VIEW) {
                if (emit_value(cc, arg, 0x7F, err, errlen) != 0) return -1;
                b_byte(o, 0x10); w_uleb(o, FUNC_PRINT_STR);
            } else if (ak == IR_BOOL) {
                if (emit_value(cc, arg, 0x7F, err, errlen) != 0) return -1;
                b_byte(o, 0x10); w_uleb(o, FUNC_PRINT_BOOL);
            } else if (ak == IR_F32) {
                if (emit_value(cc, arg, 0x7D, err, errlen) != 0) return -1;
                b_byte(o, 0xBB); // f64.promote_f32
                b_byte(o, 0x10); w_uleb(o, FUNC_PRINT_F64);
            } else if (ak == IR_F64) {
                if (emit_value(cc, arg, 0x7C, err, errlen) != 0) return -1;
                b_byte(o, 0x10); w_uleb(o, FUNC_PRINT_F64);
            } else {
                if (emit_value(cc, arg, valtype(ak), err, errlen) != 0) return -1;
                if (valtype(ak) == 0x7F && ak != IR_I64 && ak != IR_U64) {
                    b_byte(o, is_signed(ak) ? 0xAC : 0xAD);
                }
                b_byte(o, 0x10); w_uleb(o, FUNC_PRINT_I64);
            }
            return 0;
        }

        case IR_LEN: {
            if (!a0 || dst < 0) return 0;
            if (emit_value(cc, a0, 0x7F, err, errlen) != 0) return -1;
            b_byte(o, 0x10); w_uleb(o, FUNC_STRLEN);
            b_byte(o, 0xAD); // i64.extend_i32_u
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_COPY: {
            if (!a0 || dst < 0) return 0;
            if (emit_value(cc, a0, valtype(tk), err, errlen) != 0) return -1;
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        // IR_STRUCT_INIT: allocate n*8 bytes, store each field operand as i64
        // at base+i*8, then return the base pointer (i32 in wasm32).
        case IR_STRUCT_INIT: {
            if (dst < 0) return 0;
            int n = ins->operand_count;
            // alloc(n * 8) -> dst (i32 base ptr)
            b_byte(o, 0x41); w_sleb(o, n * 8);  // i32.const (n*8)
            b_byte(o, 0x10); w_uleb(o, FUNC_ALLOC);
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst); // dst = base ptr
            // store each field at base + i*8
            for (int i = 0; i < n; i++) {
                const IrOperand *fop = &ins->operands[i];
                IrTypeKind fk = IR_I64;
                if (fop->kind == IR_OP_REG && fop->reg < cc->rt->count)
                    fk = cc->rt->kinds[fop->reg];
                uint8_t fvt = valtype(fk);
                // addr = base + i*8  (i32)
                b_byte(o, 0x20); w_uleb(o, (uint64_t)dst);
                i32c(o, i * 8);
                op(o, 0x6A); // i32.add
                // value: emit as i64 (widen small int/bool to i64 for uniform 8-byte layout)
                if (emit_value(cc, fop, fvt, err, errlen) != 0) return -1;
                if (fvt == 0x7F) {
                    b_byte(o, 0xAD); // i64.extend_i32_u
                } else if (fvt == 0x7D) {
                    // f32 -> store as i32 bits reinterpreted as i64
                    b_byte(o, 0xBB); // f64.promote_f32
                    b_byte(o, 0xBD); // i64.reinterpret_f64
                } else if (fvt == 0x7C) {
                    b_byte(o, 0xBD); // i64.reinterpret_f64
                }
                st64(o); // i64.store
            }
            return 0;
        }

        // IR_FIELD_PTR: load i64 at base + field_index*8
        case IR_FIELD_PTR: {
            if (!a0 || !a1 || dst < 0) return 0;
            int fidx = (a1->kind == IR_OP_IMM) ? (int)a1->imm : 0;
            // addr = base (i32) + fidx*8
            if (emit_value(cc, a0, 0x7F, err, errlen) != 0) return -1;
            i32c(o, fidx * 8);
            op(o, 0x6A); // i32.add
            // load the i64 value
            ld64(o);
            // convert to dst type
            IrTypeKind dk = ins->dst_type.kind;
            uint8_t dvt = valtype(dk);
            if (dvt == 0x7F) {
                b_byte(o, 0xA7); // i32.wrap_i64
            } else if (dvt == 0x7C) {
                b_byte(o, 0xBF); // f64.reinterpret_i64
            } else if (dvt == 0x7D) {
                b_byte(o, 0xBF); // f64.reinterpret_i64
                b_byte(o, 0x56); // f32.demote_f64
            }
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        // IR_ARRAY_INIT: allocate n*8 bytes, store each element as i64.
        case IR_ARRAY_INIT: {
            if (dst < 0) return 0;
            int n = ins->operand_count;
            b_byte(o, 0x41); w_sleb(o, n * 8);
            b_byte(o, 0x10); w_uleb(o, FUNC_ALLOC);
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            for (int i = 0; i < n; i++) {
                const IrOperand *fop = &ins->operands[i];
                IrTypeKind fk = IR_I64;
                if (fop->kind == IR_OP_REG && fop->reg < cc->rt->count)
                    fk = cc->rt->kinds[fop->reg];
                uint8_t fvt = valtype(fk);
                b_byte(o, 0x20); w_uleb(o, (uint64_t)dst);
                i32c(o, i * 8);
                op(o, 0x6A);
                if (emit_value(cc, fop, fvt, err, errlen) != 0) return -1;
                if (fvt == 0x7F) { b_byte(o, 0xAD); }
                else if (fvt == 0x7D) { b_byte(o, 0xBB); b_byte(o, 0xBD); }
                else if (fvt == 0x7C) { b_byte(o, 0xBD); }
                st64(o);
            }
            return 0;
        }

        // IR_INDEX_PTR: load i64 at base + index*8
        case IR_INDEX_PTR: {
            if (!a0 || !a1 || dst < 0) return 0;
            // base ptr (i32)
            if (emit_value(cc, a0, 0x7F, err, errlen) != 0) return -1;
            // index (i64 or i32)
            IrTypeKind ik = IR_I64;
            if (a1->kind == IR_OP_REG && a1->reg < cc->rt->count) ik = cc->rt->kinds[a1->reg];
            if (emit_value(cc, a1, valtype(ik), err, errlen) != 0) return -1;
            if (valtype(ik) == 0x7E) { b_byte(o, 0xA7); } // i32.wrap_i64
            i32c(o, 8);
            op(o, 0x6C); // i32.mul
            op(o, 0x6A); // i32.add
            ld64(o);
            IrTypeKind dk = ins->dst_type.kind;
            uint8_t dvt = valtype(dk);
            if (dvt == 0x7F) { b_byte(o, 0xA7); }
            else if (dvt == 0x7C) { b_byte(o, 0xBF); }
            else if (dvt == 0x7D) { b_byte(o, 0xBF); b_byte(o, 0x56); }
            b_byte(o, 0x21); w_uleb(o, (uint64_t)dst);
            return 0;
        }

        case IR_BR:
        case IR_CBR:
        case IR_PHI:
            return 0; // handled by the block terminator / block prologue

        default: {
            int line = ins->line > 0 ? ins->line : 1;
            snprintf(err, errlen, "input.tiq:%d: error[E07]: wasm: unsupported IR opcode %d", line, (int)ins->op);
            return -1;
        }
    }
}

// Write phi incoming values from block `src` into successor `tgt`'s phi dsts.
static int emit_phi_moves(Buf *body, const IrFunction *f, int src, int tgt,
                          char *err, size_t errlen) {
    if (tgt < 0 || tgt >= f->block_count) return 0;
    const IrBlock *tb = &f->blocks[tgt];
    for (int p = 0; p < tb->phi_count; p++) {
        const IrPhi *phi = &tb->phis[p];
        for (int a = 0; a + 1 < phi->arg_count; a += 2) {
            const IrOperand *val = &phi->args[a];
            int pred = phi->args[a + 1].block;
            if (pred != src) continue;
            uint8_t vt = valtype(phi->type.kind);
            if (val->kind == IR_OP_REG) {
                b_byte(body, 0x20); w_uleb(body, (uint64_t)val->reg);
            } else if (val->kind == IR_OP_IMM) {
                if (vt == 0x7E) { b_byte(body, 0x42); w_sleb(body, val->imm); }
                else { b_byte(body, 0x41); w_sleb(body, val->imm); }
            } else {
                snprintf(err, errlen, "wasm: unsupported phi operand");
                return -1;
            }
            b_byte(body, 0x21); w_uleb(body, (uint64_t)phi->dst);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Per-function code body generation

typedef struct {
    const IrModule *mod;
    const IrFunction *func;
    const StrPool *pool;
    const FuncTable *ft;
    char *err;
    size_t errlen;
} FuncCtx;

static int emit_func_body(Buf *body, const FuncCtx *fc) {
    const IrFunction *f = fc->func;
    RegTypes rt;
    regtypes_build(f, &rt);
    int pc_reg = f->next_reg;

    CodeCtx cc = { body, f, &rt, fc->pool, fc->ft, f->param_count, pc_reg };

    // Locals: one (count=1,type) group per temp register, then $pc.
    {
        Buf locs;
        b_init(&locs);
        w_uleb(&locs, (uint64_t)((f->next_reg - f->param_count) + 1));
        for (int r = f->param_count; r < f->next_reg; r++) {
            IrTypeKind k = r < rt.count ? rt.kinds[r] : IR_I64;
            w_uleb(&locs, 1);
            b_byte(&locs, valtype(k));
        }
        w_uleb(&locs, 1);
        b_byte(&locs, 0x7F); // $pc i32
        b_bytes(body, locs.d, locs.len);
        b_free(&locs);
    }

    // $pc = 0 (entry block is block index 0)
    b_byte(body, 0x41); w_sleb(body, 0);
    b_byte(body, 0x21); w_uleb(body, (uint64_t)pc_reg);

    // loop $dispatch
    b_byte(body, 0x03); // loop
    b_byte(body, 0x40); // blocktype: void

    for (int bi = 0; bi < f->block_count; bi++) {
        const IrBlock *blk = &f->blocks[bi];
        // if ($pc == bi)
        b_byte(body, 0x20); w_uleb(body, (uint64_t)pc_reg);
        b_byte(body, 0x41); w_sleb(body, bi);
        b_byte(body, 0x46); // i32.eq
        b_byte(body, 0x04); // if
        b_byte(body, 0x40); // blocktype: void

        // Does this block end in a terminator?
        int has_term = 0;
        for (int i = blk->instr_start; i < blk->instr_end; i++) {
            IrOp op = f->instrs[i].op;
            if (op == IR_BR || op == IR_CBR || op == IR_RET) { has_term = 1; break; }
        }

        // Instructions
        for (int i = blk->instr_start; i < blk->instr_end; i++) {
            if (emit_instr(&cc, &f->instrs[i], fc->err, fc->errlen) != 0)
                return -1;
        }

        if (!has_term) {
            // Unterminated block: only valid as a void-function tail.
            if (f->return_type.kind != IR_VOID) {
                snprintf(fc->err, fc->errlen,
                         "wasm: non-void function '%.*s' may fall off the end",
                         (int)f->name_len, f->name);
                return -1;
            }
            b_byte(body, 0x0F); // return
        } else {
            // Phi materialization + $pc update for BR / CBR terminators.
            for (int i = blk->instr_start; i < blk->instr_end; i++) {
                const IrInstr *term = &f->instrs[i];
                if (term->op == IR_BR && term->operand_count >= 1) {
                    int tgt = term->operands[0].block;
                    if (emit_phi_moves(body, f, blk->label, tgt, fc->err, fc->errlen) != 0)
                        return -1;
                    b_byte(body, 0x41); w_sleb(body, tgt);
                    b_byte(body, 0x21); w_uleb(body, (uint64_t)pc_reg);
                    b_byte(body, 0x0C); w_uleb(body, 1); // br: back to dispatch loop
                } else if (term->op == IR_CBR && term->operand_count >= 3) {
                    const IrOperand *cond = &term->operands[0];
                    int t_tgt = term->operands[1].block;
                    int e_tgt = term->operands[2].block;
                    if (cond->kind == IR_OP_REG) {
                        b_byte(body, 0x20); w_uleb(body, (uint64_t)cond->reg);
                    } else {
                        b_byte(body, 0x41); w_sleb(body, cond->imm);
                    }
                    b_byte(body, 0x04); b_byte(body, 0x40); // if void
                    if (emit_phi_moves(body, f, blk->label, t_tgt, fc->err, fc->errlen) != 0)
                        return -1;
                    b_byte(body, 0x41); w_sleb(body, t_tgt);
                    b_byte(body, 0x21); w_uleb(body, (uint64_t)pc_reg);
                    b_byte(body, 0x05); // else
                    if (emit_phi_moves(body, f, blk->label, e_tgt, fc->err, fc->errlen) != 0)
                        return -1;
                    b_byte(body, 0x41); w_sleb(body, e_tgt);
                    b_byte(body, 0x21); w_uleb(body, (uint64_t)pc_reg);
                    b_byte(body, 0x0B); // end if
                    b_byte(body, 0x0C); w_uleb(body, 1); // br: back to dispatch loop
                }
            }
        }

        b_byte(body, 0x0B); // end if
    }

    b_byte(body, 0x0B); // end loop
    b_byte(body, 0x00); // unreachable (fallback; not reached)
    b_byte(body, 0x0B); // end function
    return 0;
}

// ---------------------------------------------------------------------------
// Module assembly

bool emit_wasm(const IrModule *module, uint8_t **out, size_t *out_len,
               char *err, size_t errlen) {
    if (!module || module->func_count < 1) {
        snprintf(err, errlen, "wasm: empty IR module");
        return false;
    }

    Buf m;
    b_init(&m);
    TypeTable tt = {0};
    StrPool pool = {0};
    FuncTable ft = {0};
    bool ok = false;

    // Runtime literals used by the print helpers.
    RtCtx rc = {
        .buf = 0, .iov = 48, .nw = 56,
        .off_true = 0, .off_false = 0,
        .off_nan = 0, .off_inf = 0, .off_ninf = 0, .off_m0 = 0, .off_0 = 0,
        .off_nl = 0,
    };
    int t0 = pool_add_cstr(&pool, "true");
    int t1 = pool_add_cstr(&pool, "false");
    int t2 = pool_add_cstr(&pool, "nan");
    int t3 = pool_add_cstr(&pool, "inf");
    int t4 = pool_add_cstr(&pool, "-inf");
    int t5 = pool_add_cstr(&pool, "-0");
    int t6 = pool_add_cstr(&pool, "0");
    int t7 = pool_add_cstr(&pool, "\n");
    if (t0 < 0 || t1 < 0 || t2 < 0 || t3 < 0 || t4 < 0 || t5 < 0 || t6 < 0 || t7 < 0) {
        snprintf(err, errlen, "wasm: internal string pool error");
        goto fail;
    }
    collect_strings(module, &pool);

    // Assign data offsets: 60-byte header (buf/iov/nw) then strings.
    {
        uint32_t total = 60;
        for (int i = 0; i < pool.count; i++) {
            pool.e[i].off = total;
            total += (uint32_t)pool.e[i].len + 1;
        }
        if (total > 65536) {
            snprintf(err, errlen, "wasm: data segment exceeds one memory page");
            goto fail;
        }
    }
    // Now that pool entries have memory offsets, assign rc offsets.
    rc.off_true = pool.e[t0].off;
    rc.off_false = pool.e[t1].off;
    rc.off_nan = pool.e[t2].off;
    rc.off_inf = pool.e[t3].off;
    rc.off_ninf = pool.e[t4].off;
    rc.off_m0 = pool.e[t5].off;
    rc.off_0 = pool.e[t6].off;
    rc.off_nl = pool.e[t7].off;

    // Runtime signatures.
    const uint8_t v_i32 = 0x7F, v_i64 = 0x7E, v_f64 = 0x7C;
    int sig_fdwrite = sig_add(&tt, (const uint8_t[]){v_i32, v_i32, v_i32, v_i32}, 4, v_i32, 1);
    int sig_proc_exit = sig_add(&tt, (const uint8_t[]){v_i32}, 1, 0, 0);
    int sig_strlen = sig_add(&tt, (const uint8_t[]){v_i32}, 1, v_i32, 1);
    int sig_print_str = sig_add(&tt, (const uint8_t[]){v_i32}, 1, 0, 0);
    int sig_print_i64 = sig_add(&tt, (const uint8_t[]){v_i64}, 1, 0, 0);
    int sig_print_bool = sig_add(&tt, (const uint8_t[]){v_i32}, 1, 0, 0);
    int sig_print_f64 = sig_add(&tt, (const uint8_t[]){v_f64}, 1, 0, 0);
    int sig_print_nl = sig_add(&tt, (const uint8_t[]){v_i32}, 1, 0, 0);
    int sig_alloc    = sig_add(&tt, (const uint8_t[]){v_i32}, 1, v_i32, 1);
    if (sig_fdwrite < 0 || sig_proc_exit < 0 || sig_strlen < 0 || sig_print_str < 0 ||
        sig_print_i64 < 0 || sig_print_bool < 0 || sig_print_f64 < 0 || sig_print_nl < 0 ||
        sig_alloc < 0)
        goto fail;

    // User function signatures + name table.
    for (int i = 0; i < module->func_count; i++) {
        const IrFunction *fn = &module->funcs[i];
        if (fn->param_count > MAX_PARAMS) {
            snprintf(err, errlen, "wasm: function '%.*s' has too many parameters",
                     (int)fn->name_len, fn->name);
            goto fail;
        }
        uint8_t params[MAX_PARAMS];
        for (int p = 0; p < fn->param_count; p++)
            params[p] = valtype(fn->param_types[p].kind);
        int hasr = fn->return_type.kind != IR_VOID;
        int si = sig_add(&tt, params, fn->param_count,
                         hasr ? valtype(fn->return_type.kind) : 0, hasr);
        if (si < 0) {
            snprintf(err, errlen, "wasm: internal signature error");
            goto fail;
        }
        ft_add(&ft, fn, si);
    }
    int start_sig = sig_add(&tt, NULL, 0, 0, 0);
    if (start_sig < 0) goto fail;

    // Header: \0asm version 1
    static const uint8_t magic[4] = {0x00, 0x61, 0x73, 0x6D};
    static const uint8_t ver[4] = {0x01, 0x00, 0x00, 0x00};
    b_bytes(&m, magic, 4);
    b_bytes(&m, ver, 4);

    // Type section.
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, (uint64_t)tt.count);
        for (int i = 0; i < tt.count; i++) {
            b_byte(&s, 0x60); // functype
            w_uleb(&s, (uint64_t)tt.s[i].nparams);
            for (int p = 0; p < tt.s[i].nparams; p++) b_byte(&s, tt.s[i].params[p]);
            w_uleb(&s, tt.s[i].has_result ? 1 : 0);
            if (tt.s[i].has_result) b_byte(&s, tt.s[i].result);
        }
        sec(&m, 1, &s);
        b_free(&s);
    }

    // Import section (wasi_snapshot_preview1: fd_write, proc_exit).
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, 2);
        w_name(&s, "wasi_snapshot_preview1", 22);
        w_name(&s, "fd_write", 8);
        b_byte(&s, 0x00); w_uleb(&s, (uint64_t)sig_fdwrite);
        w_name(&s, "wasi_snapshot_preview1", 22);
        w_name(&s, "proc_exit", 9);
        b_byte(&s, 0x00); w_uleb(&s, (uint64_t)sig_proc_exit);
        sec(&m, 2, &s);
        b_free(&s);
    }

    // Function section: type indices for local funcs (imports are 0,1).
    // Order: strlen, print_str, print_i64, print_bool, print_f64, print_nl, alloc, [user funcs], _start
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, (uint64_t)(module->func_count + 8)); // 6 print helpers + alloc + user + start
        w_uleb(&s, (uint64_t)sig_strlen);
        w_uleb(&s, (uint64_t)sig_print_str);
        w_uleb(&s, (uint64_t)sig_print_i64);
        w_uleb(&s, (uint64_t)sig_print_bool);
        w_uleb(&s, (uint64_t)sig_print_f64);
        w_uleb(&s, (uint64_t)sig_print_nl);
        w_uleb(&s, (uint64_t)sig_alloc);
        for (int i = 0; i < module->func_count; i++)
            w_uleb(&s, (uint64_t)ft.e[i].sig_idx);
        w_uleb(&s, (uint64_t)start_sig);
        sec(&m, 3, &s);
        b_free(&s);
    }

    // Memory section: one 64 KiB page.
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, 1);
        b_byte(&s, 0x00); // no max
        w_uleb(&s, 1);    // min pages
        sec(&m, 5, &s);
        b_free(&s);
    }

    // Compute data segment total for the __heap_ptr initial value.
    // heap starts immediately after the NUL-terminated string pool, 8-byte aligned.
    uint32_t data_total = 60;
    for (int i = 0; i < pool.count; i++)
        data_total += (uint32_t)pool.e[i].len + 1;
    uint32_t heap_init = (data_total + 7) & ~(uint32_t)7; // 8-byte align

    // Global section: one mutable i32 global (__heap_ptr).
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, 1);                    // one global
        b_byte(&s, 0x7F);                 // valtype i32
        b_byte(&s, 0x01);                 // mutable
        b_byte(&s, 0x41); w_sleb(&s, (int32_t)heap_init); // i32.const heap_init
        b_byte(&s, 0x0B);                 // end init expr
        sec(&m, 6, &s);
        b_free(&s);
    }

    // Export section: _start, main, memory.
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, 3);
        w_name(&s, "_start", 6);
        b_byte(&s, 0x00); w_uleb(&s, (uint64_t)(FUNC_USER_BASE + module->func_count));
        w_name(&s, "main", 4);
        b_byte(&s, 0x00); w_uleb(&s, (uint64_t)(FUNC_USER_BASE + 0));
        w_name(&s, "memory", 6);
        b_byte(&s, 0x02); w_uleb(&s, 0);
        sec(&m, 7, &s);
        b_free(&s);
    }

    // Code section.
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, (uint64_t)(module->func_count + 8));
        {
            Buf b; b_init(&b);
            emit_strlen(&b);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            emit_print_str(&b, rc.iov, rc.nw, rc.off_nl);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            emit_print_i64(&b, rc.buf, rc.iov, rc.nw, rc.off_nl);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            emit_print_bool(&b, rc.off_true, rc.off_false);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            emit_print_f64(&b, &rc);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            emit_print_nl(&b, rc.iov, rc.nw);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            emit_alloc(&b);
            w_uleb(&s, b.len); b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        for (int i = 0; i < module->func_count; i++) {
            FuncCtx fc = { module, &module->funcs[i], &pool, &ft, err, errlen };
            Buf b;
            b_init(&b);
            if (emit_func_body(&b, &fc) != 0) { b_free(&b); b_free(&s); goto fail; }
            w_uleb(&s, b.len);
            b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        {
            Buf b; b_init(&b);
            b_byte(&b, 0x00); // no locals
            b_byte(&b, 0x10); w_uleb(&b, (uint64_t)(FUNC_USER_BASE + 0)); // call main
            b_byte(&b, 0x1A);                                            // drop
            b_byte(&b, 0x41); w_sleb(&b, 0);
            b_byte(&b, 0x10); w_uleb(&b, (uint64_t)FUNC_PROC_EXIT);
            b_byte(&b, 0x0B); // end function
            w_uleb(&s, b.len);
            b_bytes(&s, b.d, b.len);
            b_free(&b);
        }
        sec(&m, 10, &s);
        b_free(&s);
    }

    // Data section: 60-byte header then NUL-terminated strings.
    {
        Buf s;
        b_init(&s);
        w_uleb(&s, 1); // one segment
        b_byte(&s, 0x00); // memory 0
        b_byte(&s, 0x41); w_sleb(&s, 0); b_byte(&s, 0x0B); // i32.const 0 end
        uint32_t total = 60;
        for (int i = 0; i < pool.count; i++)
            total += (uint32_t)pool.e[i].len + 1;
        w_uleb(&s, total);
        for (int i = 0; i < 60; i++) b_byte(&s, 0);
        for (int i = 0; i < pool.count; i++) {
            b_bytes(&s, pool.e[i].data, pool.e[i].len);
            b_byte(&s, 0);
        }
        sec(&m, 11, &s);
        b_free(&s);
    }

    *out = m.d;
    *out_len = m.len;
    ok = true;

fail:
    free(tt.s);
    pool_free(&pool);
    free(ft.e);
    if (!ok) { b_free(&m); return false; }
    return true;
}
