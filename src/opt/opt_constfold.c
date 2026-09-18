#include <stdlib.h>
#include <string.h>
#include "../../include/opt.h"

static bool is_int_imm(IrFunction *func, IrOperand *op, long long *out) {
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

static bool is_bool_imm(IrFunction *func, IrOperand *op, long long *out) {
    if (op->kind == IR_OP_IMM) {
        *out = op->imm;
        return true;
    }
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_BOOL) {
                *out = func->instrs[i].operands[0].imm;
                return true;
            }
        }
    }
    return false;
}

static bool is_float_imm(IrFunction *func, IrOperand *op, double *out) {
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_FLOAT) {
                long long bits = func->instrs[i].operands[0].imm;
                memcpy(out, &bits, sizeof(double));
                return true;
            }
        }
    }
    return false;
}

static void replace_with_int(IrInstr *instr, long long val) {
    instr->op = IR_CONST_INT;
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = val;
    instr->operand_count = 1;
}

static void replace_with_bool(IrInstr *instr, long long val) {
    instr->op = IR_CONST_BOOL;
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = val;
    instr->operand_count = 1;
}

static void replace_with_float(IrInstr *instr, double val) {
    instr->op = IR_CONST_FLOAT;
    long long bits;
    memcpy(&bits, &val, sizeof(double));
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = bits;
    instr->operand_count = 1;
}

OptResult opt_constfold(IrModule *mod, IrFunction *func) {
    (void)mod;
    OptResult result = { .changes = 0, .modified = false };

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        long long a, b;

        switch (instr->op) {
        case IR_ADD:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a + b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_SUB:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a - b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_MUL:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a * b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_DIV:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b) && b != 0) {
                replace_with_int(instr, a / b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_MOD:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b) && b != 0) {
                replace_with_int(instr, a % b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_NEG:
            if (instr->operand_count == 1 &&
                is_int_imm(func, &instr->operands[0], &a)) {
                replace_with_int(instr, -a);
                result.changes++;
                result.modified = true;
            } else {
                double fa;
                if (instr->operand_count == 1 &&
                    is_float_imm(func, &instr->operands[0], &fa)) {
                    replace_with_float(instr, -fa);
                    result.changes++;
                    result.modified = true;
                }
            }
            break;
        case IR_CMP_EQ:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a == b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_NE:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a != b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_LT:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a < b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_LE:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a <= b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_GT:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a > b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_GE:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a >= b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_AND:
            if (instr->operand_count == 2 &&
                is_bool_imm(func, &instr->operands[0], &a) &&
                is_bool_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, (a && b) ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_OR:
            if (instr->operand_count == 2 &&
                is_bool_imm(func, &instr->operands[0], &a) &&
                is_bool_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, (a || b) ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_NOT:
            if (instr->operand_count == 1 &&
                is_bool_imm(func, &instr->operands[0], &a)) {
                replace_with_bool(instr, a ? 0 : 1);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_AND:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a & b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_OR:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a | b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_XOR:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a ^ b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_SHL:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a << b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_SHR:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a >> b);
                result.changes++;
                result.modified = true;
            }
            break;
        default:
            break;
        }

        // Float arithmetic folding
        if ((instr->op == IR_ADD || instr->op == IR_SUB ||
             instr->op == IR_MUL || instr->op == IR_DIV) &&
            instr->dst_type.kind == IR_F64) {
            double fa, fb;
            if (instr->operand_count == 2 &&
                is_float_imm(func, &instr->operands[0], &fa) &&
                is_float_imm(func, &instr->operands[1], &fb)) {
                double rv = 0;
                switch (instr->op) {
                case IR_ADD: rv = fa + fb; break;
                case IR_SUB: rv = fa - fb; break;
                case IR_MUL: rv = fa * fb; break;
                case IR_DIV: rv = fa / fb; break;
                default: break;
                }
                replace_with_float(instr, rv);
                result.changes++;
                result.modified = true;
            }
        }

        // CBR folding: constant condition -> unconditional branch
        if (instr->op == IR_CBR && instr->operand_count == 3 &&
            instr->operands[0].kind == IR_OP_REG) {
            long long cond_val;
            if (is_bool_imm(func, &instr->operands[0], &cond_val)) {
                int target = cond_val ? instr->operands[1].block
                                      : instr->operands[2].block;
                instr->op = IR_BR;
                instr->operands[0].kind = IR_OP_BLOCK;
                instr->operands[0].block = target;
                instr->operand_count = 1;
                result.changes++;
                result.modified = true;
            }
        }
    }

    return result;
}
