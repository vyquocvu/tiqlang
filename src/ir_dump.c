// M17.1: Textual IR dumper for debugging and testing.
#include <stdio.h>
#include "../include/ir.h"

static const char *type_name(IrType type) {
    switch (type.kind) {
        case IR_VOID: return "void";
        case IR_BOOL: return "bool";
        case IR_I8: return "i8";
        case IR_I16: return "i16";
        case IR_I32: return "i32";
        case IR_I64: return "i64";
        case IR_U8: return "u8";
        case IR_U16: return "u16";
        case IR_U32: return "u32";
        case IR_U64: return "u64";
        case IR_F32: return "f32";
        case IR_F64: return "f64";
        case IR_STR: return "str";
        case IR_STR_VIEW: return "str_view";
        case IR_ARRAY: return "array";
        case IR_SLICE: return "slice";
        case IR_STRUCT: return type.semantic && type.semantic->struct_name ? type.semantic->struct_name : "struct";
        case IR_OPTION: return "option";
        case IR_RESULT: return "result";
        case IR_REF: return "ref";
        case IR_REF_MUT: return "ref_mut";
        case IR_VEC: return "vec";
        case IR_MAP: return "map";
        case IR_STRBUF: return "strbuf";
        default: return "?";
    }
}

static const char *op_name(IrOp op) {
    static const char *names[] = {
        [IR_CONST_INT] = "const_int",
        [IR_CONST_FLOAT] = "const_float",
        [IR_CONST_BOOL] = "const_bool",
        [IR_CONST_STR] = "const_str",
        [IR_ADD] = "add",
        [IR_SUB] = "sub",
        [IR_MUL] = "mul",
        [IR_DIV] = "div",
        [IR_MOD] = "mod",
        [IR_NEG] = "neg",
        [IR_CMP_EQ] = "cmp_eq",
        [IR_CMP_NE] = "cmp_ne",
        [IR_CMP_LT] = "cmp_lt",
        [IR_CMP_LE] = "cmp_le",
        [IR_CMP_GT] = "cmp_gt",
        [IR_CMP_GE] = "cmp_ge",
        [IR_AND] = "and",
        [IR_OR] = "or",
        [IR_NOT] = "not",
        [IR_BIT_AND] = "bit_and",
        [IR_BIT_OR] = "bit_or",
        [IR_BIT_XOR] = "bit_xor",
        [IR_BIT_SHL] = "bit_shl",
        [IR_BIT_SHR] = "bit_shr",
        [IR_BR] = "br",
        [IR_CBR] = "cbr",
        [IR_RET] = "ret",
        [IR_CALL] = "call",
        [IR_ALLOCA] = "alloca",
        [IR_LOAD] = "load",
        [IR_STORE] = "store",
        [IR_FIELD_PTR] = "field_ptr",
        [IR_INDEX_PTR] = "index_ptr",
        [IR_ARRAY_INIT] = "array_init",
        [IR_STRUCT_INIT] = "struct_init",
        [IR_PHI] = "phi",
        [IR_PRINT] = "print",
        [IR_LEN] = "len",
        [IR_COPY] = "copy"
    };
    return op < IR_OP_COUNT ? names[op] : "?";
}

static void dump_operand(FILE *out, IrOperand op) {
    switch (op.kind) {
        case IR_OP_REG: fprintf(out, "%%%d", op.reg); break;
        case IR_OP_IMM: fprintf(out, "%lld", op.imm); break;
        case IR_OP_BLOCK: fprintf(out, "bb%d", op.block); break;
        case IR_OP_STR: fprintf(out, "\"%.*s\"", (int)op.len, op.str ? op.str : ""); break;
    }
}

static void dump_function(FILE *out, IrFunction *func) {
    fprintf(out, "define %.*s(", (int)func->name_len, func->name);
    for (int i = 0; i < func->param_count; i++) {
        if (i > 0) fprintf(out, ", ");
        fprintf(out, "%%%d: %s", i, type_name(func->param_types[i]));
    }
    fprintf(out, ") -> %s {\n", type_name(func->return_type));

    for (int b = 0; b < func->block_count; b++) {
        IrBlock *block = &func->blocks[b];
        fprintf(out, "bb%d:\n", block->label);

        for (int p = 0; p < block->phi_count; p++) {
            IrPhi *phi = &block->phis[p];
            fprintf(out, "  %%%d = phi %s", phi->dst, type_name(phi->type));
            for (int i = 0; i < phi->arg_count; i += 2) {
                if (i > 0) fprintf(out, ",");
                fprintf(out, " [");
                dump_operand(out, phi->args[i]);
                fprintf(out, ", ");
                dump_operand(out, phi->args[i + 1]);
                fprintf(out, "]");
            }
            fprintf(out, "\n");
        }

        for (int i = block->instr_start; i < block->instr_end; i++) {
            IrInstr *instr = &func->instrs[i];
            fprintf(out, "  ");
            if (instr->dst >= 0) {
                fprintf(out, "%%%d = ", instr->dst);
            }
            fprintf(out, "%s", op_name(instr->op));
            if (instr->op == IR_CONST_INT || instr->op == IR_CONST_FLOAT || instr->op == IR_CONST_BOOL) {
                fprintf(out, " %s ", type_name(instr->dst_type));
                if (instr->operand_count > 0) {
                    dump_operand(out, instr->operands[0]);
                }
            } else if (instr->op == IR_CONST_STR) {
                fprintf(out, " ");
                if (instr->operand_count > 0) {
                    dump_operand(out, instr->operands[0]);
                }
            } else if (instr->op == IR_CALL) {
                if (instr->operand_count > 0) {
                    fprintf(out, " @");
                    // Print function name without quotes
                    if (instr->operands[0].kind == IR_OP_STR) {
                        fprintf(out, "%.*s", (int)instr->operands[0].len, instr->operands[0].str ? instr->operands[0].str : "");
                    } else {
                        dump_operand(out, instr->operands[0]);
                    }
                    fprintf(out, "(");
                    for (int j = 1; j < instr->operand_count; j++) {
                        if (j > 1) fprintf(out, ", ");
                        dump_operand(out, instr->operands[j]);
                    }
                    fprintf(out, ")");
                }
            } else {
                if (instr->operand_count > 0) fprintf(out, " ");
                for (int j = 0; j < instr->operand_count; j++) {
                    if (j > 0) fprintf(out, ", ");
                    dump_operand(out, instr->operands[j]);
                }
            }
            fprintf(out, "\n");
        }
    }
    fprintf(out, "}\n");
}

void ir_dump(FILE *out, const IrModule *module) {
    for (int i = 0; i < module->func_count; i++) {
        dump_function(out, &module->funcs[i]);
        if (i < module->func_count - 1) fprintf(out, "\n");
    }
}
