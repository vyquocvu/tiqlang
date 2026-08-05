// M17.2: QBE IL emitter — translates Tiq SSA IR to QBE IL text.
//
// Pipeline: IrModule -> QBE IL text file -> QBE binary -> .o -> link -> executable
//
// Design decisions:
// - Phi nodes are emitted directly (QBE supports SSA phi natively).
// - String constants go in data sections, referenced by symbol.
// - The main function is exported as $tiq_user_main (C runtime calls it).
// - User functions are prefixed tiq_fn_ to avoid C symbol collisions.
// - Print is dispatched to type-specific runtime functions using a
//   pre-pass type map (register -> IrTypeKind).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/emit_qbe.h"

// --- Type mapping ----------------------------------------------------------

static const char *qbe_ty(IrTypeKind kind) {
    switch (kind) {
        case IR_BOOL:
        case IR_I8: case IR_I16: case IR_I32:
        case IR_U8: case IR_U16: case IR_U32:
            return "w";
        case IR_I64: case IR_U64:
        case IR_STR: case IR_STR_VIEW:
        case IR_REF: case IR_REF_MUT:
            return "l";
        case IR_F32: return "s";
        case IR_F64: return "d";
        default: return "l";
    }
}

static bool is_flt(IrTypeKind k) { return k == IR_F32 || k == IR_F64; }

// --- String constant pool --------------------------------------------------

typedef struct { const char *str; size_t len; int id; } StrConst;
typedef struct { StrConst *items; int count, cap; } StrPool;

static void pool_init(StrPool *p) { p->items = NULL; p->count = p->cap = 0; }
static void pool_free(StrPool *p) { free(p->items); }

static int pool_add(StrPool *p, const char *s, size_t l) {
    for (int i = 0; i < p->count; i++)
        if (p->items[i].len == l && memcmp(p->items[i].str, s, l) == 0)
            return p->items[i].id;
    if (p->count >= p->cap) { p->cap = p->cap ? p->cap * 2 : 8; p->items = realloc(p->items, p->cap * sizeof(StrConst)); }
    int id = p->count;
    p->items[id] = (StrConst){s, l, id};
    p->count++;
    return id;
}

// --- Register type map -----------------------------------------------------
// Tracks the IR type kind of each register so we can dispatch print
// and other type-sensitive operations correctly.

#define MAX_REGS 4096

typedef struct { IrTypeKind kinds[MAX_REGS]; int count; } RegTypes;

static void regtypes_init(RegTypes *rt) { rt->count = 0; memset(rt->kinds, 0, sizeof(rt->kinds)); }

static void regtypes_set(RegTypes *rt, int reg, IrTypeKind k) {
    if (reg >= 0 && reg < MAX_REGS) {
        rt->kinds[reg] = k;
        if (reg >= rt->count) rt->count = reg + 1;
    }
}

static IrTypeKind regtypes_get(const RegTypes *rt, int reg) {
    if (reg >= 0 && reg < rt->count) return rt->kinds[reg];
    return IR_I64;  // default assumption
}

// Build the register type map by scanning all instructions in a function.
static void build_reg_types(const IrFunction *func, RegTypes *rt) {
    regtypes_init(rt);
    for (int b = 0; b < func->block_count; b++) {
        const IrBlock *blk = &func->blocks[b];
        for (int p = 0; p < blk->phi_count; p++)
            regtypes_set(rt, blk->phis[p].dst, blk->phis[p].type.kind);
        for (int i = blk->instr_start; i < blk->instr_end; i++) {
            const IrInstr *ins = &func->instrs[i];
            if (ins->dst >= 0)
                regtypes_set(rt, ins->dst, ins->dst_type.kind);
        }
    }
}

// --- String helpers --------------------------------------------------------

// The IR stores string literals with surrounding quotes (raw token text).
// Strip them so the data section contains just the content.
static void strip_quotes(const char *s, size_t len, const char **out, size_t *out_len) {
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        *out = s + 1;
        *out_len = len - 2;
    } else {
        *out = s;
        *out_len = len;
    }
}

// Add an IR string operand to the pool, stripping surrounding quotes.
static int pool_add_ir(StrPool *pool, const char *s, size_t len) {
    const char *stripped; size_t slen;
    strip_quotes(s, len, &stripped, &slen);
    return pool_add(pool, stripped, slen);
}

// --- Operand emission ------------------------------------------------------

static void emit_op(FILE *out, const IrOperand *op, StrPool *pool, int param_count) {
    switch (op->kind) {
        case IR_OP_REG:
            if (op->reg < param_count)
                fprintf(out, "%%p%d", op->reg);
            else
                fprintf(out, "%%r%d", op->reg);
            break;
        case IR_OP_IMM: fprintf(out, "%lld", op->imm); break;
        case IR_OP_BLOCK: fprintf(out, "@bb%d", op->block); break;
        case IR_OP_STR: {
            int id = pool_add_ir(pool, op->str, op->len);
            fprintf(out, "$.str%d", id);
            break;
        }
    }
}

// --- Data sections ---------------------------------------------------------

static void emit_escape_str(FILE *out, const char *s, size_t len) {
    for (size_t j = 0; j < len; j++) {
        unsigned char c = (unsigned char)s[j];
        if (c == '\\' || c == '"') fprintf(out, "\\%c", c);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c >= 32 && c < 127) fputc(c, out);
        else fprintf(out, "\\%03o", c);
    }
}

static void emit_data(FILE *out, const StrPool *pool) {
    for (int i = 0; i < pool->count; i++) {
        fprintf(out, "data $.str%d = { b \"", i);
        emit_escape_str(out, pool->items[i].str, pool->items[i].len);
        fputs("\", b 0 }\n\n", out);
    }
}

// --- Collect strings -------------------------------------------------------

static void collect_strings(const IrFunction *func, StrPool *pool) {
    for (int i = 0; i < func->instr_count; i++) {
        const IrInstr *ins = &func->instrs[i];
        if (ins->op == IR_CONST_STR && ins->operand_count > 0 &&
            ins->operands[0].kind == IR_OP_STR)
            pool_add_ir(pool, ins->operands[0].str, ins->operands[0].len);
    }
}

// --- Print dispatch --------------------------------------------------------

static const char *print_fn(IrTypeKind k) {
    switch (k) {
        case IR_STR: case IR_STR_VIEW: return "$tiq_print_str";
        case IR_BOOL: return "$tiq_print_bool";
        case IR_F32: case IR_F64: return "$tiq_print_f64";
        default: return "$tiq_print_i64";
    }
}

static const char *print_arg_ty(IrTypeKind k) {
    switch (k) {
        case IR_STR: case IR_STR_VIEW: return "l";
        case IR_F32: case IR_F64: return "d";
        case IR_BOOL: case IR_I8: case IR_I16: case IR_I32:
        case IR_U8: case IR_U16: case IR_U32: return "w";
        default: return "l";
    }
}

// Determine the QBE comparison suffix from the operand type (not the
// result type, which is always bool/w for all comparisons).
static const char *cmp_opnd_suffix(const IrOperand *op, const RegTypes *rt) {
    if (op->kind == IR_OP_REG) {
        IrTypeKind k = regtypes_get(rt, op->reg);
        if (k == IR_F32 || k == IR_F64) return "d";
    }
    return "l";
}

// --- Function emission -----------------------------------------------------

static void emit_func(FILE *out, const IrFunction *func, StrPool *pool) {
    RegTypes rt;
    build_reg_types(func, &rt);

    // Function header
    if (func->is_main) {
        fputs("export\nfunction l $tiq_user_main() {\n", out);
    } else {
        if (func->return_type.kind == IR_VOID) {
            fprintf(out, "function $tiq_fn_%.*s(", (int)func->name_len, func->name);
        } else {
            fprintf(out, "function %s $tiq_fn_%.*s(",
                    qbe_ty(func->return_type.kind),
                    (int)func->name_len, func->name);
        }
        for (int i = 0; i < func->param_count; i++) {
            if (i > 0) fputs(", ", out);
            fprintf(out, "%s %%p%d", qbe_ty(func->param_types[i].kind), i);
        }
        fputs(") {\n", out);
    }

    for (int b = 0; b < func->block_count; b++) {
        const IrBlock *blk = &func->blocks[b];
        fprintf(out, "@bb%d\n", blk->label);

        // Phi nodes
        for (int p = 0; p < blk->phi_count; p++) {
            const IrPhi *phi = &blk->phis[p];
            fprintf(out, "\t%%r%d =%s phi", phi->dst, qbe_ty(phi->type.kind));
            for (int a = 0; a < phi->arg_count; a += 2) {
                if (a > 0) fputs(",", out);
                fprintf(out, " @bb%d ", phi->args[a + 1].block);
                emit_op(out, &phi->args[a], pool, func->param_count);
            }
            fputs("\n", out);
        }

        // Instructions
        for (int i = blk->instr_start; i < blk->instr_end; i++) {
            const IrInstr *ins = &func->instrs[i];
            const char *ty = qbe_ty(ins->dst_type.kind);
            bool fl = is_flt(ins->dst_type.kind);

            switch (ins->op) {
                case IR_CONST_INT:
                case IR_CONST_BOOL:
                    if (ins->operand_count > 0)
                        fprintf(out, "\t%%r%d =%s add 0, %lld\n", ins->dst, ty, ins->operands[0].imm);
                    break;

                case IR_CONST_FLOAT:
                    if (ins->operand_count > 0) {
                        double dv; long long bits = ins->operands[0].imm;
                        memcpy(&dv, &bits, sizeof(double));
                        if (ins->dst_type.kind == IR_F32)
                            fprintf(out, "\t%%r%d =s add s_0, s_%g\n", ins->dst, dv);
                        else
                            fprintf(out, "\t%%r%d =d add d_0, d_%g\n", ins->dst, dv);
                    }
                    break;

                case IR_CONST_STR:
                    if (ins->operand_count > 0 && ins->operands[0].kind == IR_OP_STR) {
                        int id = pool_add_ir(pool, ins->operands[0].str, ins->operands[0].len);
                        fprintf(out, "\t%%r%d =l copy $.str%d\n", ins->dst, id);
                    }
                    break;

                case IR_ADD: case IR_SUB: case IR_MUL: {
                    static const char *names[] = { [IR_ADD] = "add", [IR_SUB] = "sub", [IR_MUL] = "mul" };
                    if (ins->operand_count >= 2) {
                        fprintf(out, "\t%%r%d =%s %s ", ins->dst, ty, names[ins->op]);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;
                }

                case IR_DIV:
                    if (ins->operand_count >= 2) {
                        const char *op = fl ? "div" :
                            (ins->dst_type.kind >= IR_U8 && ins->dst_type.kind <= IR_U64) ? "udiv" : "div";
                        fprintf(out, "\t%%r%d =%s %s ", ins->dst, ty, op);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_MOD:
                    if (ins->operand_count >= 2) {
                        const char *op = fl ? "rem" :
                            (ins->dst_type.kind >= IR_U8 && ins->dst_type.kind <= IR_U64) ? "urem" : "rem";
                        fprintf(out, "\t%%r%d =%s %s ", ins->dst, ty, op);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_NEG:
                    if (ins->operand_count >= 1) {
                        if (fl) {
                            fprintf(out, "\t%%r%d =%s neg ", ins->dst, ty);
                            emit_op(out, &ins->operands[0], pool, func->param_count);
                        } else {
                            fprintf(out, "\t%%r%d =%s sub 0, ", ins->dst, ty);
                            emit_op(out, &ins->operands[0], pool, func->param_count);
                        }
                        fputs("\n", out);
                    }
                    break;

                case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT: case IR_CMP_LE:
                case IR_CMP_GT: case IR_CMP_GE: {
                    // QBE: %dst =w cXXsuf a, b
                    // where XX = eq/ne/slt/sle/sgt/sge (signed int) or eq/ne/lt/le/gt/ge (float)
                    static const char *int_ops[] = {
                        [IR_CMP_EQ] = "eq", [IR_CMP_NE] = "ne",
                        [IR_CMP_LT] = "slt", [IR_CMP_LE] = "sle",
                        [IR_CMP_GT] = "sgt", [IR_CMP_GE] = "sge"
                    };
                    static const char *flt_ops[] = {
                        [IR_CMP_EQ] = "eq", [IR_CMP_NE] = "ne",
                        [IR_CMP_LT] = "lt", [IR_CMP_LE] = "le",
                        [IR_CMP_GT] = "gt", [IR_CMP_GE] = "ge"
                    };
                    if (ins->operand_count >= 2) {
                        const char *cs = cmp_opnd_suffix(&ins->operands[0], &rt);
                        const char *op = fl ? flt_ops[ins->op] : int_ops[ins->op];
                        fprintf(out, "\t%%r%d =w c%s%s ", ins->dst, op, cs);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;
                }

                case IR_AND: case IR_OR: case IR_BIT_AND: case IR_BIT_OR:
                case IR_BIT_XOR: {
                    static const char *names[] = {
                        [IR_AND] = "and", [IR_OR] = "or",
                        [IR_BIT_AND] = "and", [IR_BIT_OR] = "or", [IR_BIT_XOR] = "xor"
                    };
                    if (ins->operand_count >= 2) {
                        fprintf(out, "\t%%r%d =%s %s ", ins->dst, ty, names[ins->op]);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;
                }

                case IR_NOT:
                    if (ins->operand_count >= 1) {
                        fprintf(out, "\t%%r%d =%s xor ", ins->dst, ty);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", 1\n", out);
                    }
                    break;

                case IR_BIT_SHL:
                    if (ins->operand_count >= 2) {
                        fprintf(out, "\t%%r%d =%s shl ", ins->dst, ty);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_BIT_SHR:
                    if (ins->operand_count >= 2) {
                        fprintf(out, "\t%%r%d =%s sar ", ins->dst, ty);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_BR:
                    if (ins->operand_count >= 1)
                        fprintf(out, "\tjmp @bb%d\n", ins->operands[0].block);
                    break;

                case IR_CBR:
                    if (ins->operand_count >= 3) {
                        fputs("\tjnz ", out);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fprintf(out, ", @bb%d, @bb%d\n", ins->operands[1].block, ins->operands[2].block);
                    }
                    break;

                case IR_RET:
                    if (ins->operand_count >= 1) {
                        if (ins->operands[0].kind == IR_OP_REG) {
                            fputs("\tret ", out);
                            emit_op(out, &ins->operands[0], pool, func->param_count);
                            fputs("\n", out);
                        } else if (ins->operands[0].kind == IR_OP_IMM) {
                            fprintf(out, "\tret %lld\n", ins->operands[0].imm);
                        } else {
                            fputs("\tret\n", out);
                        }
                    } else {
                        fputs("\tret\n", out);
                    }
                    break;

                case IR_CALL:
                    if (ins->operand_count >= 1 && ins->operands[0].kind == IR_OP_STR) {
                        const char *fn = ins->operands[0].str;
                        int fl2 = (int)ins->operands[0].len;
                        if (ins->dst >= 0) {
                            fprintf(out, "\t%%r%d =%s call $tiq_fn_%.*s(", ins->dst, qbe_ty(ins->dst_type.kind), fl2, fn);
                        } else {
                            fprintf(out, "\tcall $tiq_fn_%.*s(", fl2, fn);
                        }
                        for (int a = 1; a < ins->operand_count; a++) {
                            if (a > 1) fputs(", ", out);
                            fputs("l ", out);
                            emit_op(out, &ins->operands[a], pool, func->param_count);
                        }
                        fputs(")\n", out);
                    }
                    break;

                case IR_PRINT:
                    if (ins->operand_count >= 1) {
                        const IrOperand *arg = &ins->operands[0];
                        if (arg->kind == IR_OP_STR) {
                            // Direct string constant
                            int id = pool_add_ir(pool, arg->str, arg->len);
                            fprintf(out, "\tcall $tiq_print_str(l $.str%d)\n", id);
                        } else if (arg->kind == IR_OP_REG) {
                            // Look up the type of the register
                            IrTypeKind ak = regtypes_get(&rt, arg->reg);
                            fprintf(out, "\tcall %s(%s ", print_fn(ak), print_arg_ty(ak));
                            emit_op(out, arg, pool, func->param_count);
                            fputs(")\n", out);
                        } else {
                            // Immediate — print as integer
                            fprintf(out, "\tcall $tiq_print_i64(l ");
                            emit_op(out, arg, pool, func->param_count);
                            fputs(")\n", out);
                        }
                    }
                    break;

                case IR_LEN:
                    if (ins->operand_count >= 1 && ins->dst >= 0) {
                        fprintf(out, "\t%%r%d =l call $tiq_str_len(l ", ins->dst);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(")\n", out);
                    }
                    break;

                case IR_COPY:
                    if (ins->operand_count >= 1) {
                        fprintf(out, "\t%%r%d =%s copy ", ins->dst, ty);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_ALLOCA:
                    if (ins->operand_count >= 1) {
                        fprintf(out, "\t%%r%d =l alloc8 ", ins->dst);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_LOAD:
                    if (ins->operand_count >= 1) {
                        fprintf(out, "\t%%r%d =l loadl ", ins->dst);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                case IR_STORE:
                    if (ins->operand_count >= 2) {
                        fputs("\tstorel ", out);
                        emit_op(out, &ins->operands[0], pool, func->param_count);
                        fputs(", ", out);
                        emit_op(out, &ins->operands[1], pool, func->param_count);
                        fputs("\n", out);
                    }
                    break;

                default:
                    fprintf(out, "\t# unhandled opcode %d\n", ins->op);
                    break;
            }
        }
    }
    fputs("}\n\n", out);
}

// --- Public API ------------------------------------------------------------

bool emit_qbe(FILE *out, const IrModule *module) {
    StrPool pool;
    pool_init(&pool);

    for (int i = 0; i < module->func_count; i++)
        collect_strings(&module->funcs[i], &pool);

    emit_data(out, &pool);

    for (int i = 0; i < module->func_count; i++)
        emit_func(out, &module->funcs[i], &pool);

    pool_free(&pool);
    return !ferror(out);
}
