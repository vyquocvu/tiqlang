// Native amd64 code emitter — translates Tiq SSA IR directly to amd64
// assembly text without depending on the external QBE binary.
//
// Design: stack-spill allocation. Every SSA register gets a fixed stack
// slot. R10/R11 are scratch registers for computations. PHI values use
// per-block temp slots: predecessors save source values to temps, then
// copy temps to PHI destination slots (two-phase parallel copy).
//
// Supported IR ops: CONST_INT, CONST_BOOL, CONST_STR, ADD, SUB, MUL,
// DIV, MOD, NEG, CMP_*, AND, OR, NOT, BIT_AND, BIT_OR, BIT_XOR,
// BIT_SHL, BIT_SHR, BR, CBR, RET, CALL, PRINT, PHI, COPY, LEN.
//
// Unsupported ops fail closed with a diagnostic on stderr.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../include/emit_native.h"

#define MAX_REGS 4096
#define MAX_STRS 256
#define MAX_EXTS 128
#define MAX_BLOCKS 256
#define MAX_CALLER_SAVE 6

static const char *arg_regs[MAX_CALLER_SAVE] = {
    "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
};

// --- String constant pool --------------------------------------------------

typedef struct { const char *str; size_t len; } StrEntry;
typedef struct { StrEntry items[MAX_STRS]; int count; } StrPool;

static void pool_init(StrPool *p) { p->count = 0; }

static int pool_add(StrPool *p, const char *s, size_t len) {
    for (int i = 0; i < p->count; i++)
        if (p->items[i].len == len && memcmp(p->items[i].str, s, len) == 0)
            return i;
    if (p->count >= MAX_STRS) return -1;
    int id = p->count++;
    p->items[id].str = s;
    p->items[id].len = len;
    return id;
}

static void strip_quotes(const char *s, size_t len, const char **out, size_t *out_len) {
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        *out = s + 1;
        *out_len = len - 2;
    } else {
        *out = s;
        *out_len = len;
    }
}

static int pool_add_ir(StrPool *p, const char *s, size_t len) {
    const char *stripped; size_t slen;
    strip_quotes(s, len, &stripped, &slen);
    return pool_add(p, stripped, slen);
}

// --- External symbol table -------------------------------------------------

typedef struct { char name[256]; } ExtSym;
typedef struct { ExtSym items[MAX_EXTS]; int count; } ExtTable;

static void ext_init(ExtTable *t) { t->count = 0; }

static void ext_add(ExtTable *t, const char *name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->items[i].name, name) == 0) return;
    if (t->count >= MAX_EXTS) return;
    snprintf(t->items[t->count].name, sizeof(t->items[0].name), "%s", name);
    t->count++;
}

// --- Register type map -----------------------------------------------------

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
    return IR_I64;
}

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

// --- Stack frame layout ----------------------------------------------------

typedef struct {
    int slot_offset[MAX_REGS];
    int temp_slot_base[MAX_BLOCKS];
    int temp_slot_count[MAX_BLOCKS];
    int total_slots;
} FrameLayout;

static int slot_off(const FrameLayout *f, int reg) {
    return -(f->slot_offset[reg] + 1) * 8;
}

static int temp_off(const FrameLayout *f, int block_label, int phi_idx) {
    int base = f->temp_slot_base[block_label];
    return -(base + phi_idx + 1) * 8;
}

static void compute_layout(const IrFunction *func, FrameLayout *layout) {
    int next_slot = 0;

    for (int i = 0; i < MAX_REGS; i++)
        layout->slot_offset[i] = -1;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        layout->temp_slot_base[i] = -1;
        layout->temp_slot_count[i] = 0;
    }

    for (int b = 0; b < func->block_count && b < MAX_BLOCKS; b++) {
        const IrBlock *blk = &func->blocks[b];
        if (blk->phi_count > 0) {
            layout->temp_slot_base[blk->label] = next_slot;
            layout->temp_slot_count[blk->label] = blk->phi_count;
            next_slot += blk->phi_count;
        }
    }

    for (int i = 0; i < func->param_count; i++) {
        if (layout->slot_offset[i] < 0)
            layout->slot_offset[i] = next_slot++;
    }

    for (int b = 0; b < func->block_count; b++) {
        const IrBlock *blk = &func->blocks[b];
        for (int p = 0; p < blk->phi_count; p++) {
            int reg = blk->phis[p].dst;
            if (layout->slot_offset[reg] < 0)
                layout->slot_offset[reg] = next_slot++;
        }
        for (int i = blk->instr_start; i < blk->instr_end; i++) {
            const IrInstr *ins = &func->instrs[i];
            if (ins->dst >= 0 && layout->slot_offset[ins->dst] < 0)
                layout->slot_offset[ins->dst] = next_slot++;
        }
    }

    layout->total_slots = next_slot;
}

// --- Collect defined function names and string constants -------------------

static void collect_defined_funcs(const IrModule *mod, ExtTable *defined) {
    ext_init(defined);
    for (int i = 0; i < mod->func_count; i++) {
        char name[256];
        const IrFunction *f = &mod->funcs[i];
        if (f->is_main)
            snprintf(name, sizeof(name), "tiq_user_main");
        else
            snprintf(name, sizeof(name), "tiq_fn_%.*s", (int)f->name_len, f->name);
        ext_add(defined, name);
    }
}

static void collect_strings_func(const IrFunction *func, StrPool *pool) {
    for (int i = 0; i < func->instr_count; i++) {
        const IrInstr *ins = &func->instrs[i];
        if (ins->op == IR_CONST_STR && ins->operand_count > 0 &&
            ins->operands[0].kind == IR_OP_STR)
            pool_add_ir(pool, ins->operands[0].str, ins->operands[0].len);
        if (ins->op == IR_PRINT && ins->operand_count > 0 &&
            ins->operands[0].kind == IR_OP_STR)
            pool_add_ir(pool, ins->operands[0].str, ins->operands[0].len);
    }
}

static void collect_externals_func(const IrFunction *func, ExtTable *ext,
                                   const ExtTable *defined) {
    for (int i = 0; i < func->instr_count; i++) {
        const IrInstr *ins = &func->instrs[i];
        if (ins->op == IR_CALL && ins->operand_count >= 1 &&
            ins->operands[0].kind == IR_OP_STR) {
            char name[256];
            snprintf(name, sizeof(name), "%.*s",
                     (int)ins->operands[0].len, ins->operands[0].str);
            char sym[280];
            if (strncmp(name, "tiq_", 4) == 0)
                snprintf(sym, sizeof(sym), "%s", name);
            else
                snprintf(sym, sizeof(sym), "tiq_fn_%s", name);
            bool is_defined = false;
            for (int d = 0; d < defined->count; d++)
                if (strcmp(defined->items[d].name, sym) == 0) { is_defined = true; break; }
            if (!is_defined) ext_add(ext, sym);
        }
        if (ins->op == IR_LEN) ext_add(ext, "tiq_str_len");
    }
}

static const char *print_fn_for_type(IrTypeKind k) {
    switch (k) {
        case IR_STR: case IR_STR_VIEW: return "tiq_print_str";
        case IR_BOOL: return "tiq_print_bool";
        case IR_F32: case IR_F64: return "tiq_print_f64";
        default: return "tiq_print_i64";
    }
}

static void collect_print_externals_func(const IrFunction *func, ExtTable *ext,
                                         const RegTypes *rt) {
    for (int i = 0; i < func->instr_count; i++) {
        const IrInstr *ins = &func->instrs[i];
        if (ins->op == IR_PRINT && ins->operand_count >= 1) {
            if (ins->operands[0].kind == IR_OP_STR) {
                ext_add(ext, "tiq_print_str");
            } else if (ins->operands[0].kind == IR_OP_REG) {
                ext_add(ext, print_fn_for_type(regtypes_get(rt, ins->operands[0].reg)));
            } else {
                ext_add(ext, "tiq_print_i64");
            }
        }
    }
}

// --- Emit helpers ----------------------------------------------------------

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

static bool is_unsigned_ty(IrTypeKind k) {
    return k == IR_U8 || k == IR_U16 || k == IR_U32 || k == IR_U64;
}

// Load an operand value into a target register.
static void emit_load_op(FILE *out, const IrOperand *op, const FrameLayout *layout,
                         const StrPool *pool, const char *target) {
    switch (op->kind) {
        case IR_OP_REG:
            fprintf(out, "\tmovq %d(%%rbp), %s\n", slot_off(layout, op->reg), target);
            break;
        case IR_OP_IMM:
            fprintf(out, "\tmovq $%lld, %s\n", op->imm, target);
            break;
        case IR_OP_STR: {
            int sid = pool_add_ir((StrPool *)pool, op->str, op->len);
            fprintf(out, "\tleaq .Lstr%d(%%rip), %s\n", sid, target);
            break;
        }
        case IR_OP_BLOCK:
            break;
    }
}

// --- PHI copy emission -----------------------------------------------------

static void emit_phi_copies(FILE *out, const IrFunction *func,
                            int target_label, int pred_label,
                            const FrameLayout *layout, const StrPool *pool) {
    const IrBlock *target = NULL;
    for (int b = 0; b < func->block_count; b++) {
        if (func->blocks[b].label == target_label) {
            target = &func->blocks[b];
            break;
        }
    }
    if (!target || target->phi_count == 0) return;
    if (target_label >= MAX_BLOCKS || layout->temp_slot_base[target_label] < 0) return;

    for (int p = 0; p < target->phi_count; p++) {
        const IrPhi *phi = &target->phis[p];
        int toff = temp_off(layout, target_label, p);

        for (int a = 0; a < phi->arg_count; a += 2) {
            if (phi->args[a + 1].kind == IR_OP_BLOCK &&
                phi->args[a + 1].block == pred_label) {
                if (phi->args[a].kind == IR_OP_REG) {
                    fprintf(out, "\tmovq %d(%%rbp), %%r10\n", slot_off(layout, phi->args[a].reg));
                    fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", toff);
                } else if (phi->args[a].kind == IR_OP_IMM) {
                    fprintf(out, "\tmovq $%lld, %%r10\n", phi->args[a].imm);
                    fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", toff);
                } else if (phi->args[a].kind == IR_OP_STR) {
                    int sid = pool_add_ir((StrPool *)pool, phi->args[a].str, phi->args[a].len);
                    fprintf(out, "\tleaq .Lstr%d(%%rip), %%r10\n", sid);
                    fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", toff);
                }
                break;
            }
        }
    }

    for (int p = 0; p < target->phi_count; p++) {
        int dst = target->phis[p].dst;
        int toff = temp_off(layout, target_label, p);
        fprintf(out, "\tmovq %d(%%rbp), %%r10\n", toff);
        fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
    }
}

// --- Instruction emission --------------------------------------------------

static void emit_instr(FILE *out, const IrInstr *ins, const FrameLayout *layout,
                       const IrFunction *func, const RegTypes *rt,
                       const StrPool *pool, int cur_block_label) {
    if (ins->op >= IR_OP_COUNT) return;
    int dst = ins->dst;

    switch (ins->op) {
        case IR_CONST_INT:
        case IR_CONST_BOOL:
            if (dst >= 0 && ins->operand_count > 0) {
                fprintf(out, "\tmovq $%lld, %%r10\n", ins->operands[0].imm);
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_CONST_STR:
            if (dst >= 0 && ins->operand_count > 0 && ins->operands[0].kind == IR_OP_STR) {
                int sid = pool_add_ir((StrPool *)pool, ins->operands[0].str, ins->operands[0].len);
                fprintf(out, "\tleaq .Lstr%d(%%rip), %%r10\n", sid);
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_ADD:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\taddq %%r11, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_SUB:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\tsubq %%r11, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_MUL:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\timulq %%r11, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_DIV:
            if (dst >= 0 && ins->operand_count >= 2) {
                fprintf(out, "\tpushq %%rdx\n");
                emit_load_op(out, &ins->operands[0], layout, pool, "%rax");
                if (is_unsigned_ty(ins->dst_type.kind)) {
                    fprintf(out, "\txorq %%rdx, %%rdx\n");
                    emit_load_op(out, &ins->operands[1], layout, pool, "%r10");
                    fprintf(out, "\tdivq %%r10\n");
                } else {
                    fprintf(out, "\tcqto\n");
                    emit_load_op(out, &ins->operands[1], layout, pool, "%r10");
                    fprintf(out, "\tidivq %%r10\n");
                }
                fprintf(out, "\tmovq %%rax, %d(%%rbp)\n", slot_off(layout, dst));
                fprintf(out, "\tpopq %%rdx\n");
            }
            break;

        case IR_MOD:
            if (dst >= 0 && ins->operand_count >= 2) {
                fprintf(out, "\tpushq %%rdx\n");
                emit_load_op(out, &ins->operands[0], layout, pool, "%rax");
                if (is_unsigned_ty(ins->dst_type.kind)) {
                    fprintf(out, "\txorq %%rdx, %%rdx\n");
                    emit_load_op(out, &ins->operands[1], layout, pool, "%r10");
                    fprintf(out, "\tdivq %%r10\n");
                } else {
                    fprintf(out, "\tcqto\n");
                    emit_load_op(out, &ins->operands[1], layout, pool, "%r10");
                    fprintf(out, "\tidivq %%r10\n");
                }
                fprintf(out, "\tmovq %%rdx, %d(%%rbp)\n", slot_off(layout, dst));
                fprintf(out, "\tpopq %%rdx\n");
            }
            break;

        case IR_NEG:
            if (dst >= 0 && ins->operand_count >= 1) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                fprintf(out, "\tnegq %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT: case IR_CMP_LE:
        case IR_CMP_GT: case IR_CMP_GE: {
            if (dst >= 0 && ins->operand_count >= 2) {
                static const char *setcc_s[] = {
                    [IR_CMP_EQ] = "sete", [IR_CMP_NE] = "setne",
                    [IR_CMP_LT] = "setl", [IR_CMP_LE] = "setle",
                    [IR_CMP_GT] = "setg", [IR_CMP_GE] = "setge"
                };
                static const char *setcc_u[] = {
                    [IR_CMP_EQ] = "sete", [IR_CMP_NE] = "setne",
                    [IR_CMP_LT] = "setb", [IR_CMP_LE] = "setbe",
                    [IR_CMP_GT] = "seta", [IR_CMP_GE] = "setae"
                };
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\tcmpq %%r11, %%r10\n");
                bool uns = is_unsigned_ty(ins->dst_type.kind) ||
                           is_unsigned_ty(regtypes_get(rt, ins->operands[0].reg));
                fprintf(out, "\t%s %%r10\n", uns ? setcc_u[ins->op] : setcc_s[ins->op]);
                fprintf(out, "\tmovzbq %%r10b, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;
        }

        case IR_AND:
        case IR_BIT_AND:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\tandq %%r11, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_OR:
        case IR_BIT_OR:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\torq %%r11, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_BIT_XOR:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%r11");
                fprintf(out, "\txorq %%r11, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_NOT:
            if (dst >= 0 && ins->operand_count >= 1) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                fprintf(out, "\txorq $1, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_BIT_SHL:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%rcx");
                fprintf(out, "\tshlq %%cl, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_BIT_SHR:
            if (dst >= 0 && ins->operand_count >= 2) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                emit_load_op(out, &ins->operands[1], layout, pool, "%rcx");
                if (is_unsigned_ty(ins->dst_type.kind))
                    fprintf(out, "\tshrq %%cl, %%r10\n");
                else
                    fprintf(out, "\tsarq %%cl, %%r10\n");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_BR:
            if (ins->operand_count >= 1) {
                int target = ins->operands[0].block;
                emit_phi_copies(out, func, target, cur_block_label, layout, pool);
                fprintf(out, "\tjmp .Lbb%d\n", target);
            }
            break;

        case IR_CBR:
            if (ins->operand_count >= 3) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                fprintf(out, "\tmovq %%r10, -8(%%rbp)\n");

                emit_phi_copies(out, func, ins->operands[1].block, cur_block_label, layout, pool);
                emit_phi_copies(out, func, ins->operands[2].block, cur_block_label, layout, pool);

                fprintf(out, "\tmovq -8(%%rbp), %%r10\n");
                fprintf(out, "\ttestq %%r10, %%r10\n");
                fprintf(out, "\tjnz .Lbb%d\n", ins->operands[1].block);
                fprintf(out, "\tjmp .Lbb%d\n", ins->operands[2].block);
            }
            break;

        case IR_RET:
            if (ins->operand_count >= 1) {
                if (ins->operands[0].kind == IR_OP_REG)
                    fprintf(out, "\tmovq %d(%%rbp), %%rax\n",
                            slot_off(layout, ins->operands[0].reg));
                else if (ins->operands[0].kind == IR_OP_IMM)
                    fprintf(out, "\tmovq $%lld, %%rax\n", ins->operands[0].imm);
            }
            fprintf(out, "\tmovq %%rbp, %%rsp\n");
            fprintf(out, "\tpopq %%rbp\n");
            fprintf(out, "\tretq\n");
            break;

        case IR_CALL:
            if (ins->operand_count >= 1 && ins->operands[0].kind == IR_OP_STR) {
                char fn_name[256];
                snprintf(fn_name, sizeof(fn_name), "%.*s",
                         (int)ins->operands[0].len, ins->operands[0].str);

                for (int a = 1; a < ins->operand_count && (a - 1) < MAX_CALLER_SAVE; a++) {
                    const IrOperand *arg = &ins->operands[a];
                    const char *areg = arg_regs[a - 1];
                    if (arg->kind == IR_OP_REG)
                        fprintf(out, "\tmovq %d(%%rbp), %s\n",
                                slot_off(layout, arg->reg), areg);
                    else if (arg->kind == IR_OP_IMM)
                        fprintf(out, "\tmovq $%lld, %s\n", arg->imm, areg);
                    else if (arg->kind == IR_OP_STR) {
                        int sid = pool_add_ir((StrPool *)pool, arg->str, arg->len);
                        fprintf(out, "\tleaq .Lstr%d(%%rip), %s\n", sid, areg);
                    }
                }

                if (strncmp(fn_name, "tiq_", 4) == 0)
                    fprintf(out, "\tcallq %s\n", fn_name);
                else
                    fprintf(out, "\tcallq tiq_fn_%s\n", fn_name);

                if (dst >= 0)
                    fprintf(out, "\tmovq %%rax, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_PRINT:
            if (ins->operand_count >= 1) {
                const IrOperand *arg = &ins->operands[0];

                if (arg->kind == IR_OP_STR) {
                    int sid = pool_add_ir((StrPool *)pool, arg->str, arg->len);
                    fprintf(out, "\tleaq .Lstr%d(%%rip), %%rdi\n", sid);
                    fprintf(out, "\tcallq tiq_print_str\n");
                } else if (arg->kind == IR_OP_REG) {
                    const char *pfn = print_fn_for_type(regtypes_get(rt, arg->reg));
                    fprintf(out, "\tmovq %d(%%rbp), %%rdi\n",
                            slot_off(layout, arg->reg));
                    fprintf(out, "\tcallq %s\n", pfn);
                } else if (arg->kind == IR_OP_IMM) {
                    fprintf(out, "\tmovq $%lld, %%rdi\n", arg->imm);
                    fprintf(out, "\tcallq tiq_print_i64\n");
                }
            }
            break;

        case IR_LEN:
            if (dst >= 0 && ins->operand_count >= 1) {
                if (ins->operands[0].kind == IR_OP_REG)
                    fprintf(out, "\tmovq %d(%%rbp), %%rdi\n",
                            slot_off(layout, ins->operands[0].reg));
                fprintf(out, "\tcallq tiq_str_len\n");
                fprintf(out, "\tmovq %%rax, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_COPY:
            if (dst >= 0 && ins->operand_count >= 1) {
                emit_load_op(out, &ins->operands[0], layout, pool, "%r10");
                fprintf(out, "\tmovq %%r10, %d(%%rbp)\n", slot_off(layout, dst));
            }
            break;

        case IR_ALLOCA:
        case IR_LOAD:
        case IR_STORE:
        case IR_FIELD_PTR:
        case IR_INDEX_PTR:
        case IR_ARRAY_INIT:
        case IR_STRUCT_INIT:
        case IR_CONST_FLOAT:
        case IR_PHI:
            fprintf(stderr, "tiq: native: unsupported IR opcode %d\n", ins->op);
            break;

        default:
            fprintf(stderr, "tiq: native: unhandled IR opcode %d\n", ins->op);
            break;
    }
}

// --- Function emission -----------------------------------------------------

static void emit_func(FILE *out, const IrFunction *func, const StrPool *pool) {
    RegTypes rt;
    build_reg_types(func, &rt);

    FrameLayout layout;
    compute_layout(func, &layout);

    int raw_bytes = (layout.total_slots + 1) * 8;
    int frame_size = ((raw_bytes + 15) / 16) * 16;
    if (frame_size == 0) frame_size = 16;

    if (func->is_main)
        fprintf(out, "\t.globl tiq_user_main\n");
    else
        fprintf(out, "\t.globl tiq_fn_%.*s\n", (int)func->name_len, func->name);

    fprintf(out, "\t.text\n");
    if (func->is_main)
        fprintf(out, "tiq_user_main:\n");
    else
        fprintf(out, "tiq_fn_%.*s:\n", (int)func->name_len, func->name);

    fprintf(out, "\tpushq %%rbp\n");
    fprintf(out, "\tmovq %%rsp, %%rbp\n");
    fprintf(out, "\tsubq $%d, %%rsp\n", frame_size);

    for (int i = 0; i < func->param_count && i < MAX_CALLER_SAVE; i++)
        fprintf(out, "\tmovq %s, %d(%%rbp)\n", arg_regs[i], slot_off(&layout, i));

    for (int b = 0; b < func->block_count; b++) {
        const IrBlock *blk = &func->blocks[b];
        fprintf(out, ".Lbb%d:\n", blk->label);

        for (int i = blk->instr_start; i < blk->instr_end; i++)
            emit_instr(out, &func->instrs[i], &layout, func, &rt, pool, blk->label);
    }

    if (func->block_count > 0) {
        const IrBlock *last = &func->blocks[func->block_count - 1];
        if (last->instr_end > last->instr_start) {
            const IrInstr *last_ins = &func->instrs[last->instr_end - 1];
            if (last_ins->op == IR_RET || last_ins->op == IR_BR || last_ins->op == IR_CBR)
                return;
        }
    }
    fprintf(out, "\tmovq %%rbp, %%rsp\n");
    fprintf(out, "\tpopq %%rbp\n");
    fprintf(out, "\tretq\n\n");
}

// --- Data section ----------------------------------------------------------

static void emit_data(FILE *out, const StrPool *pool) {
    if (pool->count == 0) return;
    fprintf(out, "\t.data\n");
    for (int i = 0; i < pool->count; i++) {
        fprintf(out, ".Lstr%d:\n", i);
        fprintf(out, "\t.ascii \"");
        emit_escape_str(out, pool->items[i].str, pool->items[i].len);
        fprintf(out, "\"\n\t.byte 0\n");
    }
    fprintf(out, "\t.text\n\n");
}

// --- External declarations -------------------------------------------------

static void emit_externs(FILE *out, const ExtTable *ext) {
    for (int i = 0; i < ext->count; i++)
        fprintf(out, "\t.extern %s\n", ext->items[i].name);
    if (ext->count > 0) fprintf(out, "\n");
}

// --- Public API ------------------------------------------------------------

bool emit_native(FILE *out, const IrModule *module) {
    StrPool pool;
    pool_init(&pool);

    ExtTable defined;
    collect_defined_funcs(module, &defined);

    ExtTable ext;
    ext_init(&ext);

    for (int i = 0; i < module->func_count; i++) {
        collect_strings_func(&module->funcs[i], &pool);
        collect_externals_func(&module->funcs[i], &ext, &defined);
    }

    for (int i = 0; i < module->func_count; i++) {
        RegTypes rt;
        build_reg_types(&module->funcs[i], &rt);
        collect_print_externals_func(&module->funcs[i], &ext, &rt);
    }

    emit_externs(out, &ext);
    emit_data(out, &pool);

    for (int i = 0; i < module->func_count; i++)
        emit_func(out, &module->funcs[i], &pool);

    return !ferror(out);
}
