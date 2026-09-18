#include <stdlib.h>
#include <string.h>
#include "../../include/opt.h"

static bool has_side_effects(IrOp op) {
    switch (op) {
    case IR_CALL:
    case IR_PRINT:
    case IR_STORE:
    case IR_RET:
    case IR_BR:
    case IR_CBR:
        return true;
    default:
        return false;
    }
}

OptResult opt_dce(IrModule *mod, IrFunction *func) {
    (void)mod;
    OptResult result = { .changes = 0, .modified = false };

    int *use_count = calloc((size_t)func->next_reg, sizeof(int));
    if (!use_count) return result;

    // Count uses in regular instructions
    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        for (int j = 0; j < instr->operand_count; j++) {
            if (instr->operands[j].kind == IR_OP_REG) {
                int reg = instr->operands[j].reg;
                if (reg >= 0 && reg < func->next_reg) {
                    use_count[reg]++;
                }
            }
        }
    }

    // Count uses in PHI nodes
    for (int b = 0; b < func->block_count; b++) {
        IrBlock *blk = &func->blocks[b];
        for (int p = 0; p < blk->phi_count; p++) {
            // PHI args are (reg, block) pairs; registers at even indices
            for (int a = 0; a < blk->phis[p].arg_count; a += 2) {
                if (blk->phis[p].args[a].kind == IR_OP_REG) {
                    int reg = blk->phis[p].args[a].reg;
                    if (reg >= 0 && reg < func->next_reg) {
                        use_count[reg]++;
                    }
                }
            }
        }
    }

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        if (instr->dst >= 0 && use_count[instr->dst] == 0 &&
            !has_side_effects(instr->op)) {
            instr->op = IR_OP_COUNT;
            result.changes++;
            result.modified = true;
        }
    }

    free(use_count);

    return result;
}
