#include <stdlib.h>
#include <string.h>
#include "../../include/opt.h"

static bool is_int_imm_reg(IrFunction *func, IrOperand *op, long long *out) {
    if (op->kind == IR_OP_IMM) {
        *out = op->imm;
        return true;
    }
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_INT) {
                *out = func->instrs[i].operands[0].imm;
                return true;
            }
        }
    }
    return false;
}

static void replace_with_copy(IrInstr *instr, int src_reg) {
    instr->op = IR_COPY;
    instr->operands[0].kind = IR_OP_REG;
    instr->operands[0].reg = src_reg;
    instr->operand_count = 1;
}

static void replace_with_int(IrInstr *instr, long long val) {
    instr->op = IR_CONST_INT;
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = val;
    instr->operand_count = 1;
}

OptResult opt_algebraic(IrModule *mod, IrFunction *func) {
    (void)mod;
    OptResult result = { .changes = 0, .modified = false };

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        if (instr->op == IR_OP_COUNT) continue;

        long long a, b;

        switch (instr->op) {
        case IR_ADD:
            if (instr->operand_count == 2) {
                if (is_int_imm_reg(func, &instr->operands[1], &b) && b == 0) {
                    replace_with_copy(instr, instr->operands[0].reg);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[0], &a) && a == 0) {
                    replace_with_copy(instr, instr->operands[1].reg);
                    result.changes++;
                    result.modified = true;
                }
            }
            break;
        case IR_SUB:
            if (instr->operand_count == 2 &&
                is_int_imm_reg(func, &instr->operands[1], &b) && b == 0) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_MUL:
            if (instr->operand_count == 2) {
                if (is_int_imm_reg(func, &instr->operands[1], &b) && b == 1) {
                    replace_with_copy(instr, instr->operands[0].reg);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[0], &a) && a == 1) {
                    replace_with_copy(instr, instr->operands[1].reg);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[1], &b) && b == 0) {
                    replace_with_int(instr, 0);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[0], &a) && a == 0) {
                    replace_with_int(instr, 0);
                    result.changes++;
                    result.modified = true;
                }
            }
            break;
        case IR_DIV:
            if (instr->operand_count == 2 &&
                is_int_imm_reg(func, &instr->operands[1], &b) && b == 1) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_AND:
            if (instr->operand_count == 2 &&
                instr->operands[0].kind == IR_OP_REG &&
                instr->operands[1].kind == IR_OP_REG &&
                instr->operands[0].reg == instr->operands[1].reg) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_OR:
            if (instr->operand_count == 2 &&
                instr->operands[0].kind == IR_OP_REG &&
                instr->operands[1].kind == IR_OP_REG &&
                instr->operands[0].reg == instr->operands[1].reg) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_XOR:
            if (instr->operand_count == 2 &&
                instr->operands[0].kind == IR_OP_REG &&
                instr->operands[1].kind == IR_OP_REG &&
                instr->operands[0].reg == instr->operands[1].reg) {
                replace_with_int(instr, 0);
                result.changes++;
                result.modified = true;
            }
            break;
        default:
            break;
        }
    }

    return result;
}
