// M17.1: AST-to-IR lowering pass.
// Converts typed AST to SSA-based IR.
#include <stdlib.h>
#include <string.h>
#include "../include/ir.h"
#include "../include/ast.h"

#define MAX_VARS 256
#define MAX_SCOPES 32
#define MAX_LOOPS 16

typedef struct {
    const char *name;
    size_t name_len;
    int reg;
    IrType type;
    int scope_level;
} VarEntry;

typedef struct {
    int header;
    int exit_block;
} LoopEntry;

typedef struct {
    IrModule *module;
    IrFunction *func;
    int current_block;
    DiagContext *diag;
    VarEntry vars[MAX_VARS];
    int var_count;
    int scope_level;
    LoopEntry loop_stack[MAX_LOOPS];
    int loop_depth;
} LowerCtx;

static int find_var(LowerCtx *ctx, const char *name, size_t len) {
    for (int i = ctx->var_count - 1; i >= 0; i--) {
        if (ctx->vars[i].name_len == len && strncmp(ctx->vars[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_var(LowerCtx *ctx, const char *name, size_t len, int reg, IrType type) {
    int idx = find_var(ctx, name, len);
    if (idx >= 0) {
        ctx->vars[idx].reg = reg;
        ctx->vars[idx].type = type;
    } else {
        ctx->vars[ctx->var_count++] = (VarEntry){name, len, reg, type, ctx->scope_level};
    }
}

static void enter_scope(LowerCtx *ctx) {
    ctx->scope_level++;
}

static void exit_scope(LowerCtx *ctx) {
    while (ctx->var_count > 0 && ctx->vars[ctx->var_count - 1].scope_level == ctx->scope_level) {
        ctx->var_count--;
    }
    ctx->scope_level--;
}

static IrOperand reg_op(int reg) {
    return (IrOperand){IR_OP_REG, .reg = reg};
}

static IrOperand block_op(int block) {
    return (IrOperand){IR_OP_BLOCK, .block = block};
}

static IrOperand str_op(const char *str, size_t len) {
    IrOperand op;
    op.kind = IR_OP_STR;
    op.str = str;
    op.len = len;
    return op;
}

static IrType void_type(void) {
    return (IrType){IR_VOID, NULL};
}

static IrType bool_type(void) {
    return (IrType){IR_BOOL, NULL};
}

static IrType i64_type(void) {
    return (IrType){IR_I64, NULL};
}

// Forward declarations
static int lower_expr(LowerCtx *ctx, AstNode *node);
static void lower_stmt(LowerCtx *ctx, AstNode *node);
static void lower_block_stmts(LowerCtx *ctx, AstNode **stmts, int count);

static int lower_expr(LowerCtx *ctx, AstNode *node) {
    if (!node) return -1;

    switch (node->kind) {
        case AST_LITERAL: {
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            if (node->as.literal.type == TOK_INT) {
                long long val = strtoll(node->token.start, NULL, 10);
                IrOperand ops[] = {(IrOperand){IR_OP_IMM, .imm = val}};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CONST_INT, dst, type, ops, 1, node->token.line);
            } else if (node->as.literal.type == TOK_FLOAT) {
                double val = strtod(node->token.start, NULL);
                IrOperand ops[] = {(IrOperand){IR_OP_IMM, .imm = *(long long*)&val}};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CONST_FLOAT, dst, type, ops, 1, node->token.line);
            } else if (node->as.literal.type == TOK_TRUE || node->as.literal.type == TOK_FALSE) {
                IrOperand ops[] = {(IrOperand){IR_OP_IMM, .imm = (node->as.literal.type == TOK_TRUE)}};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CONST_BOOL, dst, type, ops, 1, node->token.line);
            } else if (node->as.literal.type == TOK_STRING) {
                IrOperand ops[] = {str_op(node->token.start, node->token.length)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CONST_STR, dst, type, ops, 1, node->token.line);
            }
            return dst;
        }

        case AST_IDENTIFIER: {
            int idx = find_var(ctx, node->as.identifier.name.start, node->as.identifier.name.length);
            if (idx < 0) return -1;
            return ctx->vars[idx].reg;
        }

        case AST_BINARY: {
            int left = lower_expr(ctx, node->as.binary.left);
            int right = lower_expr(ctx, node->as.binary.right);
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            IrOp op;
            switch (node->as.binary.op) {
                case TOK_PLUS: op = IR_ADD; break;
                case TOK_MINUS: op = IR_SUB; break;
                case TOK_STAR: op = IR_MUL; break;
                case TOK_SLASH: op = IR_DIV; break;
                case TOK_PERCENT: op = IR_MOD; break;
                case TOK_EQ_EQ: op = IR_CMP_EQ; break;
                case TOK_BANG_EQ: op = IR_CMP_NE; break;
                case TOK_LT: op = IR_CMP_LT; break;
                case TOK_LTE: op = IR_CMP_LE; break;
                case TOK_GT: op = IR_CMP_GT; break;
                case TOK_GTE: op = IR_CMP_GE; break;
                case TOK_AND_AND: op = IR_AND; break;
                case TOK_OR_OR: op = IR_OR; break;
                case TOK_AMP: op = IR_BIT_AND; break;
                case TOK_PIPE: op = IR_BIT_OR; break;
                case TOK_CARET: op = IR_BIT_XOR; break;
                case TOK_LSHIFT: op = IR_BIT_SHL; break;
                case TOK_RSHIFT: op = IR_BIT_SHR; break;
                default: return -1;
            }
            IrOperand ops[] = {reg_op(left), reg_op(right)};
            ir_emit_into_block(ctx->func, ctx->current_block, op, dst, type, ops, 2, node->token.line);
            return dst;
        }

        case AST_UNARY: {
            int right = lower_expr(ctx, node->as.unary.right);
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            IrOp op;
            if (node->as.unary.op == TOK_MINUS) op = IR_NEG;
            else if (node->as.unary.op == TOK_BANG) op = IR_NOT;
            else return -1;
            IrOperand ops[] = {reg_op(right)};
            ir_emit_into_block(ctx->func, ctx->current_block, op, dst, type, ops, 1, node->token.line);
            return dst;
        }

        case AST_CONDITIONAL: {
            int cond = lower_expr(ctx, node->as.conditional.cond);
            int then_block_idx = ir_add_block(ctx->func, ctx->func->block_count);
            int else_block_idx = ir_add_block(ctx->func, ctx->func->block_count);
            int merge_block_idx = ir_add_block(ctx->func, ctx->func->block_count);
            IrOperand cbr_ops[] = {reg_op(cond), block_op(then_block_idx), block_op(else_block_idx)};
            ir_emit_into_block(ctx->func, ctx->current_block, IR_CBR, -1, void_type(), cbr_ops, 3, node->token.line);

            ctx->current_block = then_block_idx;
            int then_reg = lower_expr(ctx, node->as.conditional.then_branch);
            int then_end_block = ctx->current_block;
            IrOperand br_then[] = {block_op(merge_block_idx)};
            ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_then, 1, node->token.line);

            ctx->current_block = else_block_idx;
            int else_reg = lower_expr(ctx, node->as.conditional.else_branch);
            int else_end_block = ctx->current_block;
            IrOperand br_else[] = {block_op(merge_block_idx)};
            ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_else, 1, node->token.line);

            ctx->current_block = merge_block_idx;
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            IrOperand phi_args[] = {reg_op(then_reg), block_op(then_end_block), reg_op(else_reg), block_op(else_end_block)};
            ir_emit_phi(ctx->func, merge_block_idx, dst, type, phi_args, 4);
            return dst;
        }

        case AST_CALL: {
            if (node->as.call.callee->kind == AST_IDENTIFIER) {
                const char *name = node->as.call.callee->as.identifier.name.start;
                size_t name_len = node->as.call.callee->as.identifier.name.length;
                if (name_len == 5 && memcmp(name, "print", 5) == 0 && node->as.call.arg_count == 1) {
                    int arg = lower_expr(ctx, node->as.call.args[0]);
                    IrOperand ops[] = {reg_op(arg)};
                    ir_emit_into_block(ctx->func, ctx->current_block, IR_PRINT, -1, void_type(), ops, 1, node->token.line);
                    return -1;
                }
                if (name_len == 3 && memcmp(name, "len", 3) == 0 && node->as.call.arg_count == 1) {
                    int arg = lower_expr(ctx, node->as.call.args[0]);
                    int dst = ir_new_reg(ctx->func);
                    IrOperand ops[] = {reg_op(arg)};
                    ir_emit_into_block(ctx->func, ctx->current_block, IR_LEN, dst, i64_type(), ops, 1, node->token.line);
                    return dst;
                }
            }
            // General function call
            int *arg_regs = malloc(node->as.call.arg_count * sizeof(int));
            for (int i = 0; i < node->as.call.arg_count; i++) {
                arg_regs[i] = lower_expr(ctx, node->as.call.args[i]);
            }
            int dst = -1;
            IrType type = node->semantic_type ? ir_type_from_semantic(node->semantic_type) : i64_type();
            if (type.kind != IR_VOID) dst = ir_new_reg(ctx->func);
            IrOperand *ops = malloc((1 + node->as.call.arg_count) * sizeof(IrOperand));
            ops[0] = str_op(node->as.call.callee->as.identifier.name.start, node->as.call.callee->as.identifier.name.length);
            for (int i = 0; i < node->as.call.arg_count; i++) {
                ops[i + 1] = reg_op(arg_regs[i]);
            }
            ir_emit_into_block(ctx->func, ctx->current_block, IR_CALL, dst, type, ops, 1 + node->as.call.arg_count, node->token.line);
            free(arg_regs);
            free(ops);
            return dst;
        }

        case AST_ARRAY: {
            int *elem_regs = malloc(node->as.array.element_count * sizeof(int));
            for (int i = 0; i < node->as.array.element_count; i++) {
                elem_regs[i] = lower_expr(ctx, node->as.array.elements[i]);
            }
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            IrOperand *ops = malloc(node->as.array.element_count * sizeof(IrOperand));
            for (int i = 0; i < node->as.array.element_count; i++) {
                ops[i] = reg_op(elem_regs[i]);
            }
            ir_emit_into_block(ctx->func, ctx->current_block, IR_ARRAY_INIT, dst, type, ops, node->as.array.element_count, node->token.line);
            free(elem_regs);
            free(ops);
            return dst;
        }

        case AST_BLOCK: {
            enter_scope(ctx);
            int result = -1;
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                lower_stmt(ctx, node->as.block.statements[i]);
            }
            if (node->as.block.final_expr) {
                result = lower_expr(ctx, node->as.block.final_expr);
            }
            exit_scope(ctx);
            return result;
        }

        default:
            return -1;
    }
}

static void lower_stmt(LowerCtx *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_BINDING: {
            int val_reg = lower_expr(ctx, node->as.binding.expr);
            IrType type = ir_type_from_semantic(node->semantic_type);
            set_var(ctx, node->as.binding.name.start, node->as.binding.name.length, val_reg, type);
            break;
        }

        case AST_ASSIGN: {
            int val_reg = lower_expr(ctx, node->as.assign.expr);
            IrType type = ir_type_from_semantic(node->semantic_type);
            set_var(ctx, node->as.assign.name.start, node->as.assign.name.length, val_reg, type);
            break;
        }

        case AST_BRACKET_LOOP: {
            if (node->as.bracket_loop.domain->kind == AST_BINARY &&
                node->as.bracket_loop.domain->as.binary.op == TOK_DOT_DOT) {
                // Range loop: [start..end] { body }
                int start_reg = lower_expr(ctx, node->as.bracket_loop.domain->as.binary.left);
                int end_reg = lower_expr(ctx, node->as.bracket_loop.domain->as.binary.right);

                int header_block = ir_add_block(ctx->func, ctx->func->block_count);
                int body_block = ir_add_block(ctx->func, ctx->func->block_count);
                int exit_block = ir_add_block(ctx->func, ctx->func->block_count);

                IrOperand br_hdr[] = {block_op(header_block)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_hdr, 1, node->token.line);

                ctx->current_block = header_block;
                int i_reg = ir_new_reg(ctx->func);
                IrOperand phi_args[] = {reg_op(start_reg), block_op(ctx->current_block - 1), reg_op(-1), block_op(body_block)};
                ir_emit_phi(ctx->func, header_block, i_reg, i64_type(), phi_args, 4);

                const char *var_name = node->as.bracket_loop.has_binder ?
                    node->as.bracket_loop.binder.start : "i";
                size_t var_len = node->as.bracket_loop.has_binder ?
                    node->as.bracket_loop.binder.length : 1;
                enter_scope(ctx);
                set_var(ctx, var_name, var_len, i_reg, i64_type());

                int cond_reg = ir_new_reg(ctx->func);
                IrOperand cmp_ops[] = {reg_op(i_reg), reg_op(end_reg)};
                ir_emit_into_block(ctx->func, header_block, IR_CMP_LT, cond_reg, bool_type(), cmp_ops, 2, node->token.line);

                IrOperand cbr_ops[] = {reg_op(cond_reg), block_op(body_block), block_op(exit_block)};
                ir_emit_into_block(ctx->func, header_block, IR_CBR, -1, void_type(), cbr_ops, 3, node->token.line);

                ctx->loop_stack[ctx->loop_depth++] = (LoopEntry){header_block, exit_block};

                ctx->current_block = body_block;
                lower_block_stmts(ctx, node->as.bracket_loop.body_stmts, node->as.bracket_loop.body_count);
                if (node->as.bracket_loop.body_final) {
                    lower_stmt(ctx, node->as.bracket_loop.body_final);
                }

                int next_i = ir_new_reg(ctx->func);
                IrOperand one_op[] = {(IrOperand){IR_OP_IMM, .imm = 1}};
                int one_reg = ir_new_reg(ctx->func);
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CONST_INT, one_reg, i64_type(), one_op, 1, node->token.line);
                IrOperand add_ops[] = {reg_op(i_reg), reg_op(one_reg)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_ADD, next_i, i64_type(), add_ops, 2, node->token.line);

                ctx->func->blocks[header_block].phis[0].args[2] = reg_op(next_i);

                IrOperand br_back[] = {block_op(header_block)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_back, 1, node->token.line);

                ctx->loop_depth--;
                exit_scope(ctx);
                ctx->current_block = exit_block;
            }
            break;
        }

        case AST_BREAK: {
            if (ctx->loop_depth > 0) {
                IrOperand ops[] = {block_op(ctx->loop_stack[ctx->loop_depth - 1].exit_block)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), ops, 1, node->token.line);
            }
            break;
        }

        case AST_SKIP: {
            if (ctx->loop_depth > 0) {
                IrOperand ops[] = {block_op(ctx->loop_stack[ctx->loop_depth - 1].header)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), ops, 1, node->token.line);
            }
            break;
        }

        case AST_FUNCTION: {
            // Save current function index (not pointer — realloc may move the array)
            int saved_func_idx = (int)(ctx->func - ctx->module->funcs);
            int saved_block = ctx->current_block;
            ctx->module->funcs = realloc(ctx->module->funcs, (ctx->module->func_count + 1) * sizeof(IrFunction));
            IrFunction *fn = &ctx->module->funcs[ctx->module->func_count++];
            // Create a null-terminated copy of the function name
            char *name_copy = malloc(node->as.function.name.length + 1);
            memcpy(name_copy, node->as.function.name.start, node->as.function.name.length);
            name_copy[node->as.function.name.length] = '\0';
            ir_func_init(fn, name_copy, node->as.function.name.length);
            fn->owns_name = true;
            fn->param_count = node->as.function.param_count;
            fn->param_types = malloc(fn->param_count * sizeof(IrType));
            for (int i = 0; i < fn->param_count; i++) {
                SemanticType *pt = node->as.function.param_types ? (SemanticType *)node->as.function.param_types[i] : NULL;
                fn->param_types[i] = pt ? ir_type_from_semantic(pt) : i64_type();
            }
            fn->return_type = node->semantic_type ? ir_type_from_semantic(node->semantic_type) : i64_type();
            ctx->func = fn;

            int entry_block = ir_add_block(ctx->func, 0);
            ctx->current_block = entry_block;

            enter_scope(ctx);
            for (int i = 0; i < node->as.function.param_count; i++) {
                int param_reg = ir_new_reg(ctx->func);
                set_var(ctx, node->as.function.params[i].start, node->as.function.params[i].length, param_reg, fn->param_types[i]);
            }

            if (node->as.function.body && node->as.function.body->kind == AST_BLOCK) {
                for (int i = 0; i < node->as.function.body->as.block.stmt_count; i++) {
                    lower_stmt(ctx, node->as.function.body->as.block.statements[i]);
                }
                if (node->as.function.body->as.block.final_expr) {
                    int ret_reg = lower_expr(ctx, node->as.function.body->as.block.final_expr);
                    IrOperand ops[] = {reg_op(ret_reg)};
                    ir_emit_into_block(ctx->func, ctx->current_block, IR_RET, -1, void_type(), ops, 1, node->token.line);
                }
            } else if (node->as.function.body) {
                // Body is a direct expression (e.g. `add a b -> a + b`)
                int ret_reg = lower_expr(ctx, node->as.function.body);
                if (ret_reg >= 0) {
                    IrOperand ops[] = {reg_op(ret_reg)};
                    ir_emit_into_block(ctx->func, ctx->current_block, IR_RET, -1, void_type(), ops, 1, node->token.line);
                }
            }
            exit_scope(ctx);

            // Restore func pointer from module (realloc may have moved it)
            ctx->func = &ctx->module->funcs[saved_func_idx];
            ctx->current_block = saved_block;
            break;
        }

        default: {
            lower_expr(ctx, node);
            break;
        }
    }
}

static void lower_block_stmts(LowerCtx *ctx, AstNode **stmts, int count) {
    for (int i = 0; i < count; i++) {
        lower_stmt(ctx, stmts[i]);
    }
}

bool ir_lower(AstNode **stmts, int count, IrModule *module, DiagContext *diag) {
    LowerCtx ctx;
    ctx.module = module;
    ctx.diag = diag;
    ctx.var_count = 0;
    ctx.scope_level = 0;
    ctx.loop_depth = 0;

    module->funcs = realloc(module->funcs, sizeof(IrFunction));
    module->func_count = 1;
    ir_func_init(&module->funcs[0], "main", 4);
    module->funcs[0].is_main = true;
    module->funcs[0].return_type = i64_type();
    ctx.func = &module->funcs[0];

    int entry_block = ir_add_block(ctx.func, 0);
    ctx.current_block = entry_block;

    for (int i = 0; i < count; i++) {
        if (stmts[i]->kind == AST_FUNCTION) {
            lower_stmt(&ctx, stmts[i]);
        }
    }

    for (int i = 0; i < count; i++) {
        if (stmts[i]->kind != AST_FUNCTION) {
            lower_stmt(&ctx, stmts[i]);
        }
    }

    IrOperand ret_zero[] = {(IrOperand){IR_OP_IMM, .imm = 0}};
    int zero_reg = ir_new_reg(ctx.func);
    ir_emit_into_block(ctx.func, ctx.current_block, IR_CONST_INT, zero_reg, i64_type(), ret_zero, 1, 0);
    IrOperand ret_ops[] = {reg_op(zero_reg)};
    ir_emit_into_block(ctx.func, ctx.current_block, IR_RET, -1, void_type(), ret_ops, 1, 0);

    return !diag->has_error;
}
