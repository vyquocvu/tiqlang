// M17.1: IR construction, destruction, and helper functions.
#include <stdlib.h>
#include <string.h>
#include "../include/ir.h"

#define IR_INITIAL_CAP 8

void ir_module_init(IrModule *mod) {
    mod->funcs = NULL;
    mod->func_count = 0;
    mod->func_cap = 0;
}

void ir_module_free(IrModule *mod) {
    for (int i = 0; i < mod->func_count; i++) {
        ir_func_free(&mod->funcs[i]);
    }
    free(mod->funcs);
    mod->funcs = NULL;
    mod->func_count = 0;
    mod->func_cap = 0;
}

void ir_func_init(IrFunction *func, const char *name, size_t name_len) {
    func->name = name;
    func->name_len = name_len;
    func->instrs = NULL;
    func->instr_count = 0;
    func->instr_cap = 0;
    func->blocks = NULL;
    func->block_count = 0;
    func->block_cap = 0;
    func->param_types = NULL;
    func->param_count = 0;
    func->return_type.kind = IR_VOID;
    func->return_type.semantic = NULL;
    func->next_reg = 0;
    func->is_main = false;
    func->owns_name = false;
}

void ir_func_free(IrFunction *func) {
    for (int i = 0; i < func->instr_count; i++) {
        free(func->instrs[i].operands);
    }
    free(func->instrs);
    for (int i = 0; i < func->block_count; i++) {
        for (int j = 0; j < func->blocks[i].phi_count; j++) {
            free(func->blocks[i].phis[j].args);
        }
        free(func->blocks[i].phis);
    }
    free(func->blocks);
    free(func->param_types);
    if (func->owns_name) free((void *)func->name);
    func->instrs = NULL;
    func->instr_count = 0;
    func->instr_cap = 0;
    func->blocks = NULL;
    func->block_count = 0;
    func->block_cap = 0;
    func->param_types = NULL;
    func->param_count = 0;
}

IrType ir_type_from_semantic(SemanticType *sem) {
    IrType t;
    if (!sem) {
        t.kind = IR_VOID;
        t.semantic = NULL;
        return t;
    }
    switch (sem->kind) {
        case TYPE_BOOL: t.kind = IR_BOOL; break;
        case TYPE_I8: t.kind = IR_I8; break;
        case TYPE_I16: t.kind = IR_I16; break;
        case TYPE_I32: t.kind = IR_I32; break;
        case TYPE_INT: t.kind = IR_I64; break;  // TYPE_I64 is alias
        case TYPE_U8: t.kind = IR_U8; break;
        case TYPE_U16: t.kind = IR_U16; break;
        case TYPE_U32: t.kind = IR_U32; break;
        case TYPE_U64: t.kind = IR_U64; break;
        case TYPE_FLOAT: t.kind = IR_F64; break;  // TYPE_F64 is alias
        case TYPE_F32: t.kind = IR_F32; break;
        case TYPE_STR: t.kind = IR_STR; break;
        case TYPE_STR_VIEW: t.kind = IR_STR_VIEW; break;
        case TYPE_ARRAY: t.kind = IR_ARRAY; break;
        case TYPE_SLICE: t.kind = IR_SLICE; break;
        case TYPE_STRUCT: t.kind = IR_STRUCT; break;
        case TYPE_OPTION: t.kind = IR_OPTION; break;
        case TYPE_RESULT: t.kind = IR_RESULT; break;
        case TYPE_REF: t.kind = IR_REF; break;
        case TYPE_REF_MUT: t.kind = IR_REF_MUT; break;
        case TYPE_VEC: t.kind = IR_VEC; break;
        case TYPE_MAP: t.kind = IR_MAP; break;
        case TYPE_STRBUF: t.kind = IR_STRBUF; break;
        default: t.kind = IR_VOID; break;
    }
    t.semantic = sem;
    return t;
}

static void ir_grow_instrs(IrFunction *func) {
    if (func->instr_count >= func->instr_cap) {
        func->instr_cap = func->instr_cap ? func->instr_cap * 2 : IR_INITIAL_CAP;
        func->instrs = realloc(func->instrs, func->instr_cap * sizeof(IrInstr));
    }
}

void ir_emit_instr(IrFunction *func, IrOp op, int dst, IrType dst_type,
                   IrOperand *operands, int operand_count, int line) {
    ir_grow_instrs(func);
    IrInstr *instr = &func->instrs[func->instr_count++];
    instr->op = op;
    instr->dst = dst;
    instr->dst_type = dst_type;
    instr->operands = NULL;
    instr->operand_count = 0;
    instr->line = line;
    if (operand_count > 0 && operands) {
        instr->operands = malloc(operand_count * sizeof(IrOperand));
        memcpy(instr->operands, operands, operand_count * sizeof(IrOperand));
        instr->operand_count = operand_count;
    }
}

void ir_emit_into_block(IrFunction *func, int block_idx, IrOp op, int dst,
                        IrType dst_type, IrOperand *operands, int operand_count, int line) {
    ir_grow_instrs(func);
    IrInstr *instr = &func->instrs[func->instr_count++];
    instr->op = op;
    instr->dst = dst;
    instr->dst_type = dst_type;
    instr->operands = NULL;
    instr->operand_count = 0;
    instr->line = line;
    if (operand_count > 0 && operands) {
        instr->operands = malloc(operand_count * sizeof(IrOperand));
        memcpy(instr->operands, operands, operand_count * sizeof(IrOperand));
        instr->operand_count = operand_count;
    }
    IrBlock *block = &func->blocks[block_idx];
    if (block->instr_start == block->instr_end) {
        block->instr_start = func->instr_count - 1;
    }
    block->instr_end = func->instr_count;
}

void ir_emit_phi(IrFunction *func, int block_idx, int dst, IrType type,
                 IrOperand *args, int arg_count) {
    IrBlock *block = &func->blocks[block_idx];
    block->phis = realloc(block->phis, (block->phi_count + 1) * sizeof(IrPhi));
    IrPhi *phi = &block->phis[block->phi_count++];
    phi->dst = dst;
    phi->type = type;
    phi->args = NULL;
    phi->arg_count = 0;
    phi->block = block_idx;
    if (arg_count > 0 && args) {
        phi->args = malloc(arg_count * sizeof(IrOperand));
        memcpy(phi->args, args, arg_count * sizeof(IrOperand));
        phi->arg_count = arg_count;
    }
}

int ir_add_block(IrFunction *func, int label) {
    if (func->block_count >= func->block_cap) {
        func->block_cap = func->block_cap ? func->block_cap * 2 : IR_INITIAL_CAP;
        func->blocks = realloc(func->blocks, func->block_cap * sizeof(IrBlock));
    }
    int idx = func->block_count++;
    IrBlock *block = &func->blocks[idx];
    block->label = label;
    block->instr_start = func->instr_count;
    block->instr_end = func->instr_count;
    block->phis = NULL;
    block->phi_count = 0;
    return idx;
}

int ir_new_reg(IrFunction *func) {
    return func->next_reg++;
}
