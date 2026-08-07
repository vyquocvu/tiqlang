// M17.1: SSA-based Intermediate Representation for Tiq.
// The IR sits between the typed AST and code generation backends.
// It is backend-agnostic (no QBE/x86/LLVM specifics) and covers the full
// checked language surface: scalars, structs, arrays, control flow, functions.
#ifndef TIQ_IR_H
#define TIQ_IR_H

#include <stdio.h>
#include <stdbool.h>
#include "semantic.h"

// IR opcodes: operations on SSA temporaries.
typedef enum {
    // Constants
    IR_CONST_INT,       // dst = immediate integer
    IR_CONST_FLOAT,     // dst = immediate float
    IR_CONST_BOOL,      // dst = immediate bool
    IR_CONST_STR,       // dst = immediate string (pointer in operand[0].str)

    // Arithmetic (typed by dst_type.kind)
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_NEG,

    // Comparison (result is bool)
    IR_CMP_EQ, IR_CMP_NE, IR_CMP_LT, IR_CMP_LE, IR_CMP_GT, IR_CMP_GE,

    // Logical
    IR_AND, IR_OR, IR_NOT,

    // Bitwise
    IR_BIT_AND, IR_BIT_OR, IR_BIT_XOR, IR_BIT_SHL, IR_BIT_SHR,

    // Control flow
    IR_BR,              // unconditional branch: operand[0].block
    IR_CBR,             // conditional branch: operand[0].reg cond, operand[1].block then, operand[2].block else
    IR_RET,             // return: operand[0].reg (or no operand for void)

    // Function calls
    IR_CALL,            // dst = call operand[0].func, args in operand[1..n]

    // Memory/structs/arrays
    IR_ALLOCA,          // dst = allocate space for type
    IR_LOAD,            // dst = load from operand[0].reg address
    IR_STORE,           // store operand[1].reg into operand[0].reg address
    IR_FIELD_PTR,       // dst = pointer to field operand[1].field_idx of operand[0].reg struct
    IR_INDEX_PTR,       // dst = pointer to operand[1].reg index of operand[0].reg array

    // Aggregates
    IR_ARRAY_INIT,      // dst = array literal, elements in operand[0..n]
    IR_STRUCT_INIT,     // dst = struct literal, fields in operand[0..n]

    // Phi nodes (SSA merge)
    IR_PHI,             // dst = phi type [operand[0].reg, operand[0].block], [operand[1].reg, operand[1].block], ...

    // Builtins (initially map to opcodes; backend decides how to emit)
    IR_PRINT,           // print operand[0].reg
    IR_LEN,             // dst = length of operand[0].reg (array/slice/string)

    // Copy (for variable renaming)
    IR_COPY,            // dst = operand[0].reg

    IR_OP_COUNT
} IrOp;

// IR type kinds (mirrors PrimitiveType but for IR-level tracking).
typedef enum {
    IR_VOID = 0,
    IR_BOOL,
    IR_I8, IR_I16, IR_I32, IR_I64,
    IR_U8, IR_U16, IR_U32, IR_U64,
    IR_F32, IR_F64,
    IR_STR, IR_STR_VIEW,
    IR_ARRAY, IR_SLICE, IR_STRUCT,
    IR_OPTION, IR_RESULT,
    IR_REF, IR_REF_MUT,
    IR_VEC, IR_MAP, IR_STRBUF,
    IR_TYPE_COUNT
} IrTypeKind;

// IR type: scalar kind + optional composite metadata.
typedef struct {
    IrTypeKind kind;
    SemanticType *semantic;  // for composite types: points to the interned SemanticType
} IrType;

// Operand: tagged union for register, constant, block label, or string.
typedef struct {
    enum { IR_OP_REG, IR_OP_IMM, IR_OP_BLOCK, IR_OP_STR } kind;
    union {
        int reg;             // IR_OP_REG: source register
        long long imm;       // IR_OP_IMM: immediate integer/float bits
        int block;           // IR_OP_BLOCK: basic block index
        struct {             // IR_OP_STR: string constant
            const char *str;
            size_t len;
        };
    };
} IrOperand;

// IR instruction: opcode + destination + type + operands.
typedef struct {
    IrOp op;
    int dst;                 // destination register (-1 for void)
    IrType dst_type;         // type of dst
    IrOperand *operands;     // operand array (owned; freed with instruction)
    int operand_count;
    int line;                // source line for diagnostics
} IrInstr;

// Phi node: dst = phi type [reg, block], [reg, block], ...
typedef struct {
    int dst;
    IrType type;
    IrOperand *args;         // (reg, block) pairs; operand_count = 2 * arg_count
    int arg_count;
    int block;               // block this phi belongs to
} IrPhi;

// Basic block: label + instruction range + phi list.
typedef struct {
    int label;               // block number (bb0, bb1, ...)
    int instr_start;         // index into function's instrs array
    int instr_end;           // exclusive
    IrPhi *phis;             // phi nodes for this block
    int phi_count;
} IrBlock;

// IR function: name + instructions + blocks + register allocator.
typedef struct {
    const char *name;
    size_t name_len;
    IrInstr *instrs;
    int instr_count;
    int instr_cap;
    IrBlock *blocks;
    int block_count;
    int block_cap;
    IrType *param_types;
    int param_count;
    IrType return_type;
    int next_reg;            // next available register
    bool is_main;            // true for the implicit main function
    bool owns_name;          // true if name was malloc'd and should be freed
} IrFunction;

// IR module: collection of functions.
typedef struct {
    IrFunction *funcs;
    int func_count;
    int func_cap;
} IrModule;

// API: module lifecycle
void ir_module_init(IrModule *mod);
void ir_module_free(IrModule *mod);

// API: function lifecycle
void ir_func_init(IrFunction *func, const char *name, size_t name_len);
void ir_func_free(IrFunction *func);

// API: type conversion
IrType ir_type_from_semantic(SemanticType *sem);

// API: instruction emission (append to current function)
void ir_emit_instr(IrFunction *func, IrOp op, int dst, IrType dst_type,
                   IrOperand *operands, int operand_count, int line);
// Emit an instruction into a specific block (for out-of-order block filling).
void ir_emit_into_block(IrFunction *func, int block_idx, IrOp op, int dst,
                        IrType dst_type, IrOperand *operands, int operand_count, int line);
void ir_emit_phi(IrFunction *func, int block, int dst, IrType type,
                 IrOperand *args, int arg_count);

// API: basic blocks
int ir_add_block(IrFunction *func, int label);

// API: register allocation
int ir_new_reg(IrFunction *func);

// API: lowering from typed AST to IR
bool ir_lower(AstNode **stmts, int count, IrModule *module, DiagContext *diag, const char *path);

// API: textual IR dumper
void ir_dump(FILE *out, const IrModule *module);

#endif
