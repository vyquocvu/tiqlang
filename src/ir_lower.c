// M17.1: AST-to-IR lowering pass.
// Converts typed AST to SSA-based IR.
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../include/ir.h"
#include "../include/ast.h"

#define MAX_VARS 256
#define MAX_SCOPES 32
#define MAX_LOOPS 16
#define MAX_STREAM_GENS 64
#define MAX_ENUMS 64

typedef struct {
    const char *name;
    size_t name_len;
    int reg;
    IrType type;
    int scope_level;
} VarEntry;

typedef struct {
    int header;
    int inc_block;
    int exit_block;
} LoopEntry;

typedef struct {
    char *name;
    size_t name_len;
    Token *variants;
    int variant_count;
} EnumEntryLower;

// M17.4.3: top-level stream generators ([... a + b]) lower into ordinary
// IR functions named by the user symbol, with signature (params..., n) -> i64
// mirroring the C backend's tiq_gen_<name> (emit_c.c emit_stream_gen_def).
typedef struct {
    char *name;            // malloc'd, null-terminated
    size_t name_len;
    AstNode *gen;          // AST_STREAM_GEN node
    Token *params;         // user params (NULL for bindings)
    int param_count;
} StreamGenEntry;

typedef struct {
    IrModule *module;
    IrFunction *func;
    int current_block;
    DiagContext *diag;
    const char *path;
    VarEntry vars[MAX_VARS];
    int var_count;
    int scope_level;
    LoopEntry loop_stack[MAX_LOOPS];
    int loop_depth;
    StreamGenEntry stream_gens[MAX_STREAM_GENS];
    int stream_gen_count;
    EnumEntryLower enums[MAX_ENUMS];
    int enum_count;
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
        if (ctx->var_count >= MAX_VARS) {
            diag_error(ctx->diag, ctx->path, 0, ERR_UNSUPPORTED_STATEMENT,
                       "too many local variables (limit 256)");
            return;
        }
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

// M17.4: IR lowering cannot represent some valid Tiq constructs (structs,
// stream generators, field access, matches, ...). These must fail closed with
// a located diagnostic rather than silently emitting a garbage register.
static int lower_unsupported(LowerCtx *ctx, AstNode *node, const char *what) {
    diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, what);
    return -1;
}

static IrType i64_type(void) {
    return (IrType){IR_I64, NULL};
}

// Forward declarations
static int lower_expr(LowerCtx *ctx, AstNode *node);
static void lower_stmt(LowerCtx *ctx, AstNode *node);
static void lower_block_stmts(LowerCtx *ctx, AstNode **stmts, int count);
static void lower_stream_gen(LowerCtx *ctx, StreamGenEntry *gen);

static StreamGenEntry *find_stream_gen(LowerCtx *ctx, const char *name, size_t len) {
    for (int i = 0; i < ctx->stream_gen_count; i++) {
        if (ctx->stream_gens[i].name_len == len &&
            memcmp(ctx->stream_gens[i].name, name, len) == 0) {
            return &ctx->stream_gens[i];
        }
    }
    return NULL;
}

static int emit_const_i64(LowerCtx *ctx, int block, long long val, int line) {
    int dst = ir_new_reg(ctx->func);
    IrOperand ops[] = {(IrOperand){IR_OP_IMM, .imm = val}};
    ir_emit_into_block(ctx->func, block, IR_CONST_INT, dst, i64_type(), ops, 1, line);
    return dst;
}

static int lower_expr(LowerCtx *ctx, AstNode *node) {
    if (!node) return -1;

    switch (node->kind) {
        case AST_LITERAL: {
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            if (node->as.literal.type == TOK_INT) {
                // Lexer allows underscore digit separators (1_000); strip them
                // before parsing because strtoll stops at '_'.
                char tmp[64];
                size_t tl = 0;
                for (size_t i = 0; i < node->token.length && tl < sizeof(tmp) - 1; i++)
                    if (node->token.start[i] != '_') tmp[tl++] = node->token.start[i];
                tmp[tl] = '\0';
                long long val = strtoll(tmp, NULL, 10);
                IrOperand ops[] = {(IrOperand){IR_OP_IMM, .imm = val}};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CONST_INT, dst, type, ops, 1, node->token.line);
            } else if (node->as.literal.type == TOK_FLOAT) {
                // Strip underscore digit separators before strtod.
                char tmp[64];
                size_t tl = 0;
                for (size_t i = 0; i < node->token.length && tl < sizeof(tmp) - 1; i++)
                    if (node->token.start[i] != '_') tmp[tl++] = node->token.start[i];
                tmp[tl] = '\0';
                double val = strtod(tmp, NULL);
                long long bits;
                memcpy(&bits, &val, sizeof(bits));
                IrOperand ops[] = {(IrOperand){IR_OP_IMM, .imm = bits}};
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
            int merge_block_idx = ir_add_block(ctx->func, ctx->func->block_count);
            if (!node->as.conditional.else_branch) {
                // M25: one-arm conditional (`cond ? then`). The value is
                // discarded (type unit), so the false edge jumps straight to
                // the merge block and no phi is produced.
                IrOperand cbr_ops[] = {reg_op(cond), block_op(then_block_idx), block_op(merge_block_idx)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_CBR, -1, void_type(), cbr_ops, 3, node->token.line);
                ctx->current_block = then_block_idx;
                lower_expr(ctx, node->as.conditional.then_branch);
                IrOperand br_then[] = {block_op(merge_block_idx)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_then, 1, node->token.line);
                ctx->current_block = merge_block_idx;
                return -1;
            }
            int else_block_idx = ir_add_block(ctx->func, ctx->func->block_count);
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
            // M17.4.3: flatten parameterized stream bracket calls such as
            // `evens(10)[5]` (outer bracket call over an inner call whose
            // callee is a stream generator) into a single IR_CALL with the
            // inner args followed by the index args.
            if (node->as.call.is_bracket_call && node->as.call.callee &&
                node->as.call.callee->kind == AST_CALL &&
                node->as.call.callee->as.call.callee &&
                node->as.call.callee->as.call.callee->kind == AST_IDENTIFIER) {
                AstNode *inner = node->as.call.callee;
                AstNode *inner_callee = inner->as.call.callee;
                if (find_stream_gen(ctx, inner_callee->as.identifier.name.start,
                                    inner_callee->as.identifier.name.length)) {
                    int total = inner->as.call.arg_count + node->as.call.arg_count;
                    int *arg_regs = malloc(total * sizeof(int));
                    for (int i = 0; i < inner->as.call.arg_count; i++) {
                        arg_regs[i] = lower_expr(ctx, inner->as.call.args[i]);
                    }
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        arg_regs[inner->as.call.arg_count + i] = lower_expr(ctx, node->as.call.args[i]);
                    }
                    int dst = -1;
                    IrType type = node->semantic_type ? ir_type_from_semantic(node->semantic_type) : i64_type();
                    if (type.kind != IR_VOID) dst = ir_new_reg(ctx->func);
                    IrOperand *ops = malloc((1 + total) * sizeof(IrOperand));
                    ops[0] = str_op(inner_callee->as.identifier.name.start, inner_callee->as.identifier.name.length);
                    for (int i = 0; i < total; i++) {
                        ops[i + 1] = reg_op(arg_regs[i]);
                    }
                    ir_emit_into_block(ctx->func, ctx->current_block, IR_CALL, dst, type, ops, 1 + total, node->token.line);
                    free(arg_regs);
                    free(ops);
                    return dst;
                }
            }
            if (node->as.call.callee->kind != AST_IDENTIFIER) {
                return lower_unsupported(ctx, node,
                    "calls with a non-identifier callee (e.g. indexing a call result) are not supported by IR lowering yet");
            }
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

        case AST_RECORD_LIT: {
            int fc = node->as.record_lit.field_count;
            int *elem_regs = malloc(fc * sizeof(int));
            for (int i = 0; i < fc; i++) {
                elem_regs[i] = lower_expr(ctx, node->as.record_lit.field_values[i]);
            }
            int dst = ir_new_reg(ctx->func);
            IrType type = ir_type_from_semantic(node->semantic_type);
            IrOperand *ops = malloc(fc * sizeof(IrOperand));
            for (int i = 0; i < fc; i++) {
                ops[i] = reg_op(elem_regs[i]);
            }
            ir_emit_into_block(ctx->func, ctx->current_block, IR_STRUCT_INIT, dst, type, ops, fc, node->token.line);
            free(elem_regs);
            free(ops);
            return dst;
        }

        case AST_FIELD_ACCESS: {
            AstNode *target = node->as.field_access.target;
            if (target && target->kind == AST_IDENTIFIER) {
                for (int e = 0; e < ctx->enum_count; e++) {
                    if (ctx->enums[e].name_len == target->as.identifier.name.length &&
                        memcmp(ctx->enums[e].name, target->as.identifier.name.start, ctx->enums[e].name_len) == 0) {
                        for (int v = 0; v < ctx->enums[e].variant_count; v++) {
                            if (ctx->enums[e].variants[v].length == node->as.field_access.field.length &&
                                memcmp(ctx->enums[e].variants[v].start, node->as.field_access.field.start, node->as.field_access.field.length) == 0) {
                                return emit_const_i64(ctx, ctx->current_block, v, node->token.line);
                            }
                        }
                    }
                }
            }
            SemanticType *target_sem = target ? (SemanticType *)target->semantic_type : NULL;
            if (target_sem && target_sem->kind == TYPE_STRUCT) {
                int target_reg = lower_expr(ctx, target);
                int field_idx = -1;
                for (int i = 0; i < target_sem->field_count; i++) {
                    if ((int)node->as.field_access.field.length == (int)strlen(target_sem->field_names[i]) &&
                        memcmp(node->as.field_access.field.start, target_sem->field_names[i], node->as.field_access.field.length) == 0) {
                        field_idx = i;
                        break;
                    }
                }
                if (field_idx >= 0) {
                    int dst = ir_new_reg(ctx->func);
                    IrType type = ir_type_from_semantic(node->semantic_type);
                    IrOperand ops[] = {reg_op(target_reg), (IrOperand){IR_OP_IMM, .imm = field_idx}};
                    ir_emit_into_block(ctx->func, ctx->current_block, IR_FIELD_PTR, dst, type, ops, 2, node->token.line);
                    return dst;
                }
            }
            return lower_unsupported(ctx, node, "unsupported field access in IR lowering");
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
            return lower_unsupported(ctx, node, "construct is not supported by IR lowering yet");
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
                int inc_block = ir_add_block(ctx->func, ctx->func->block_count);
                int exit_block = ir_add_block(ctx->func, ctx->func->block_count);

                IrOperand br_hdr[] = {block_op(header_block)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_hdr, 1, node->token.line);

                ctx->current_block = header_block;
                int i_reg = ir_new_reg(ctx->func);
                IrOperand phi_args[] = {reg_op(start_reg), block_op(ctx->current_block - 1), reg_op(-1), block_op(inc_block)};
                ir_emit_phi(ctx->func, header_block, i_reg, i64_type(), phi_args, 4);

                // M22: bare range loops use a mangled internal variable name.
                const char *var_name = node->as.bracket_loop.has_binder ?
                    node->as.bracket_loop.binder.start : "_tiq_i";
                size_t var_len = node->as.bracket_loop.has_binder ?
                    node->as.bracket_loop.binder.length : 7;
                enter_scope(ctx);
                set_var(ctx, var_name, var_len, i_reg, i64_type());

                int cond_reg = ir_new_reg(ctx->func);
                IrOperand cmp_ops[] = {reg_op(i_reg), reg_op(end_reg)};
                ir_emit_into_block(ctx->func, header_block, IR_CMP_LT, cond_reg, bool_type(), cmp_ops, 2, node->token.line);

                IrOperand cbr_ops[] = {reg_op(cond_reg), block_op(body_block), block_op(exit_block)};
                ir_emit_into_block(ctx->func, header_block, IR_CBR, -1, void_type(), cbr_ops, 3, node->token.line);

                if (ctx->loop_depth >= MAX_LOOPS) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                               "loops nested too deeply (limit 16)");
                } else {
                    ctx->loop_stack[ctx->loop_depth++] = (LoopEntry){header_block, inc_block, exit_block};
                }

                ctx->current_block = body_block;
                lower_block_stmts(ctx, node->as.bracket_loop.body_stmts, node->as.bracket_loop.body_count);
                if (node->as.bracket_loop.body_final) {
                    lower_stmt(ctx, node->as.bracket_loop.body_final);
                }

                // Normal fall-through: body -> increment block.
                IrOperand br_inc[] = {block_op(inc_block)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), br_inc, 1, node->token.line);

                // Increment block: next = i + 1, back to the header. This is the
                // single back-edge, so the header phi reads a computed value even
                // when `skip` jumps here past dead body code (wasm executes IR
                // linearly; QBE hoists the def on its own).
                ctx->current_block = inc_block;
                int next_i = ir_new_reg(ctx->func);
                IrOperand one_op[] = {(IrOperand){IR_OP_IMM, .imm = 1}};
                int one_reg = ir_new_reg(ctx->func);
                ir_emit_into_block(ctx->func, inc_block, IR_CONST_INT, one_reg, i64_type(), one_op, 1, node->token.line);
                IrOperand add_ops[] = {reg_op(i_reg), reg_op(one_reg)};
                ir_emit_into_block(ctx->func, inc_block, IR_ADD, next_i, i64_type(), add_ops, 2, node->token.line);

                ctx->func->blocks[header_block].phis[0].args[2] = reg_op(next_i);

                IrOperand br_back[] = {block_op(header_block)};
                ir_emit_into_block(ctx->func, inc_block, IR_BR, -1, void_type(), br_back, 1, node->token.line);

                ctx->loop_depth--;
                exit_scope(ctx);
                ctx->current_block = exit_block;
            }
            break;
        }

        case AST_BREAK: {
            if (ctx->loop_depth > 0) {
                int exit = ctx->loop_stack[ctx->loop_depth - 1].exit_block;
                IrOperand ops[] = {block_op(exit)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), ops, 1, node->token.line);
                // Any statements after a terminator are dead but must still be
                // lowered into a fresh block: the wasm backend dispatches each
                // block linearly, so instructions may not follow a br.
                ctx->current_block = ir_add_block(ctx->func, ctx->func->block_count);
            }
            break;
        }

        case AST_SKIP: {
            if (ctx->loop_depth > 0) {
                int inc = ctx->loop_stack[ctx->loop_depth - 1].inc_block;
                IrOperand ops[] = {block_op(inc)};
                ir_emit_into_block(ctx->func, ctx->current_block, IR_BR, -1, void_type(), ops, 1, node->token.line);
                ctx->current_block = ir_add_block(ctx->func, ctx->func->block_count);
            }
            break;
        }

        case AST_FUNCTION: {
            // Save current function index (not pointer — realloc may move the array)
            int saved_func_idx = (int)(ctx->func - ctx->module->funcs);
            int saved_block = ctx->current_block;
            void *tmp_funcs = realloc(ctx->module->funcs, (ctx->module->func_count + 1) * sizeof(IrFunction));
            if (!tmp_funcs) { diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "out of memory"); return; }
            ctx->module->funcs = tmp_funcs;
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

        case AST_STRUCT_DEF:
        case AST_ENUM_DEF:
            // Type-level definitions require no statement runtime code in IR.
            break;

        default: {
            // Expression statement (e.g. a bare `print(...)` call) or an
            // unsupported construct; lower_expr fails closed on the latter.
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

// M17.4.3: lower a top-level stream generator into an IR function whose
// signature is (user params..., index n) -> i64, mirroring the C backend's
// tiq_gen_<name> (emit_c.c emit_stream_gen_def). The generator runs a
// header/body/inc loop: the accumulator (x, or the window a/b) is a header
// phi whose back-edge value is materialized in the inc block via IR_COPY so
// both the wasm phi-move emission and QBE's SSA rules see a value defined in
// the predecessor block.
static void lower_stream_gen(LowerCtx *ctx, StreamGenEntry *gen) {
    AstNode *node = gen->gen;
    int sc = node->as.stream_gen.seed_count;
    int line = node->token.line;
    IrFunction *fn;
    IrModule *m = ctx->module;

    if (sc < 1) {
        lower_unsupported(ctx, node, "stream generators without seeds are not supported by IR lowering yet");
        return;
    }

    void *tmp_funcs = realloc(m->funcs, (m->func_count + 1) * sizeof(IrFunction));
    if (!tmp_funcs) { diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "out of memory"); return; }
    m->funcs = tmp_funcs;
    fn = &m->funcs[m->func_count++];
    ir_func_init(fn, gen->name, gen->name_len);
    fn->owns_name = true;
    fn->param_count = gen->param_count + 1;
    fn->param_types = malloc(fn->param_count * sizeof(IrType));
    for (int i = 0; i < fn->param_count; i++) fn->param_types[i] = i64_type();
    fn->return_type = i64_type();
    ctx->func = fn;

    int entry = ir_add_block(fn, 0);
    ctx->current_block = entry;
    enter_scope(ctx);

    for (int p = 0; p < gen->param_count; p++) {
        int r = ir_new_reg(fn);
        set_var(ctx, gen->params[p].start, gen->params[p].length, r, i64_type());
    }
    int n_reg = ir_new_reg(fn);

    // if (n < 0) return 0;
    int zero_reg = emit_const_i64(ctx, entry, 0, line);
    int neg = ir_new_reg(fn);
    IrOperand neg_ops[] = {reg_op(n_reg), reg_op(zero_reg)};
    ir_emit_into_block(fn, entry, IR_CMP_LT, neg, bool_type(), neg_ops, 2, line);
    int ret0_blk = ir_add_block(fn, fn->block_count);
    int guard_blk = ir_add_block(fn, fn->block_count);
    IrOperand cbr0[] = {reg_op(neg), block_op(ret0_blk), block_op(guard_blk)};
    ir_emit_into_block(fn, entry, IR_CBR, -1, void_type(), cbr0, 3, line);
    IrOperand ret0[] = {reg_op(zero_reg)};
    ir_emit_into_block(fn, ret0_blk, IR_RET, -1, void_type(), ret0, 1, line);

    int one_reg = emit_const_i64(ctx, guard_blk, 1, line);
    ctx->current_block = guard_blk;

    if (sc == 1) {
        // if (n == 0) return x; x = seed
        int x_seed = lower_expr(ctx, node->as.stream_gen.seeds[0]);
        int eq = ir_new_reg(fn);
        IrOperand eq_ops[] = {reg_op(n_reg), reg_op(zero_reg)};
        ir_emit_into_block(fn, guard_blk, IR_CMP_EQ, eq, bool_type(), eq_ops, 2, line);
        int retx_blk = ir_add_block(fn, fn->block_count);
        int header = ir_add_block(fn, fn->block_count);
        IrOperand cbr_eq[] = {reg_op(eq), block_op(retx_blk), block_op(header)};
        ir_emit_into_block(fn, guard_blk, IR_CBR, -1, void_type(), cbr_eq, 3, line);
        IrOperand retx[] = {reg_op(x_seed)};
        ir_emit_into_block(fn, retx_blk, IR_RET, -1, void_type(), retx, 1, line);

        int i_phi = ir_new_reg(fn);
        int x_phi = ir_new_reg(fn);
        // M22: use explicit binder name instead of hardcoded "x".
        const char *xn = (node->as.stream_gen.binder_count > 0) ?
            node->as.stream_gen.binders[0].start : "x";
        int xn_len = (node->as.stream_gen.binder_count > 0) ?
            (int)node->as.stream_gen.binders[0].length : 1;
        set_var(ctx, xn, xn_len, x_phi, i64_type());
        set_var(ctx, "i", 1, i_phi, i64_type());
        int body = ir_add_block(fn, fn->block_count);
        int inc = ir_add_block(fn, fn->block_count);
        int exit_blk = ir_add_block(fn, fn->block_count);
        ctx->current_block = header;
        int le = ir_new_reg(fn);
        IrOperand le_ops[] = {reg_op(i_phi), reg_op(n_reg)};
        ir_emit_into_block(fn, header, IR_CMP_LE, le, bool_type(), le_ops, 2, line);
        IrOperand cbr_le[] = {reg_op(le), block_op(body), block_op(exit_blk)};
        ir_emit_into_block(fn, header, IR_CBR, -1, void_type(), cbr_le, 3, line);

        ctx->current_block = body;
        int next_x = lower_expr(ctx, node->as.stream_gen.gen_expr);
        IrOperand br_inc[] = {block_op(inc)};
        ir_emit_into_block(fn, body, IR_BR, -1, void_type(), br_inc, 1, line);

        ctx->current_block = inc;
        int inc_x = ir_new_reg(fn);
        IrOperand cpy[] = {reg_op(next_x)};
        ir_emit_into_block(fn, inc, IR_COPY, inc_x, i64_type(), cpy, 1, line);
        int next_i = ir_new_reg(fn);
        IrOperand add_ops[] = {reg_op(i_phi), reg_op(one_reg)};
        ir_emit_into_block(fn, inc, IR_ADD, next_i, i64_type(), add_ops, 2, line);
        IrOperand br_hdr[] = {block_op(header)};
        ir_emit_into_block(fn, inc, IR_BR, -1, void_type(), br_hdr, 1, line);

        ctx->current_block = exit_blk;
        IrOperand ret_x[] = {reg_op(x_phi)};
        ir_emit_into_block(fn, exit_blk, IR_RET, -1, void_type(), ret_x, 1, line);

        IrOperand i_args[] = {reg_op(one_reg), block_op(guard_blk), reg_op(next_i), block_op(inc)};
        ir_emit_phi(fn, header, i_phi, i64_type(), i_args, 4);
        IrOperand x_args[] = {reg_op(x_seed), block_op(guard_blk), reg_op(inc_x), block_op(inc)};
        ir_emit_phi(fn, header, x_phi, i64_type(), x_args, 4);
    } else {
        // if (n == 0) return seed0; if (n == 1) return seed1;
        int eq0 = ir_new_reg(fn);
        IrOperand eq0_ops[] = {reg_op(n_reg), reg_op(zero_reg)};
        ir_emit_into_block(fn, guard_blk, IR_CMP_EQ, eq0, bool_type(), eq0_ops, 2, line);
        int ret0_blk2 = ir_add_block(fn, fn->block_count);
        int guard1 = ir_add_block(fn, fn->block_count);
        IrOperand cbr0b[] = {reg_op(eq0), block_op(ret0_blk2), block_op(guard1)};
        ir_emit_into_block(fn, guard_blk, IR_CBR, -1, void_type(), cbr0b, 3, line);
        ctx->current_block = ret0_blk2;
        int seed0r = lower_expr(ctx, node->as.stream_gen.seeds[0]);
        IrOperand r0[] = {reg_op(seed0r)};
        ir_emit_into_block(fn, ret0_blk2, IR_RET, -1, void_type(), r0, 1, line);

        int eq1 = ir_new_reg(fn);
        IrOperand eq1_ops[] = {reg_op(n_reg), reg_op(one_reg)};
        ir_emit_into_block(fn, guard1, IR_CMP_EQ, eq1, bool_type(), eq1_ops, 2, line);
        int ret1_blk = ir_add_block(fn, fn->block_count);
        int seed_def = ir_add_block(fn, fn->block_count);
        IrOperand cbr1[] = {reg_op(eq1), block_op(ret1_blk), block_op(seed_def)};
        ir_emit_into_block(fn, guard1, IR_CBR, -1, void_type(), cbr1, 3, line);
        ctx->current_block = ret1_blk;
        int seed1r = lower_expr(ctx, node->as.stream_gen.seeds[1]);
        IrOperand r1[] = {reg_op(seed1r)};
        ir_emit_into_block(fn, ret1_blk, IR_RET, -1, void_type(), r1, 1, line);

        // a = seed1; b = seed0; then loop from i = 2.
        ctx->current_block = seed_def;
        int a_seed = lower_expr(ctx, node->as.stream_gen.seeds[1]);
        int b_seed = lower_expr(ctx, node->as.stream_gen.seeds[0]);
        int two_reg = emit_const_i64(ctx, seed_def, 2, line);
        int header = ir_add_block(fn, fn->block_count);
        IrOperand br_hdr2[] = {block_op(header)};
        ir_emit_into_block(fn, seed_def, IR_BR, -1, void_type(), br_hdr2, 1, line);

        int i_phi = ir_new_reg(fn);
        int a_phi = ir_new_reg(fn);
        int b_phi = ir_new_reg(fn);
        // M22: use explicit binder names instead of hardcoded "a"/"b".
        const char *an = (node->as.stream_gen.binder_count > 0) ?
            node->as.stream_gen.binders[0].start : "a";
        int an_len = (node->as.stream_gen.binder_count > 0) ?
            (int)node->as.stream_gen.binders[0].length : 1;
        const char *bn = (node->as.stream_gen.binder_count > 1) ?
            node->as.stream_gen.binders[1].start : "b";
        int bn_len = (node->as.stream_gen.binder_count > 1) ?
            (int)node->as.stream_gen.binders[1].length : 1;
        set_var(ctx, an, an_len, a_phi, i64_type());
        set_var(ctx, bn, bn_len, b_phi, i64_type());
        set_var(ctx, "i", 1, i_phi, i64_type());
        int body = ir_add_block(fn, fn->block_count);
        int inc = ir_add_block(fn, fn->block_count);
        int exit_blk = ir_add_block(fn, fn->block_count);
        ctx->current_block = header;
        int le = ir_new_reg(fn);
        IrOperand le_ops[] = {reg_op(i_phi), reg_op(n_reg)};
        ir_emit_into_block(fn, header, IR_CMP_LE, le, bool_type(), le_ops, 2, line);
        IrOperand cbr_le[] = {reg_op(le), block_op(body), block_op(exit_blk)};
        ir_emit_into_block(fn, header, IR_CBR, -1, void_type(), cbr_le, 3, line);

        ctx->current_block = body;
        int t_reg = lower_expr(ctx, node->as.stream_gen.gen_expr);
        int b_next = ir_new_reg(fn);
        IrOperand cpy_b[] = {reg_op(a_phi)};
        ir_emit_into_block(fn, body, IR_COPY, b_next, i64_type(), cpy_b, 1, line);
        int a_next = ir_new_reg(fn);
        IrOperand cpy_a[] = {reg_op(t_reg)};
        ir_emit_into_block(fn, body, IR_COPY, a_next, i64_type(), cpy_a, 1, line);
        IrOperand br_inc[] = {block_op(inc)};
        ir_emit_into_block(fn, body, IR_BR, -1, void_type(), br_inc, 1, line);

        ctx->current_block = inc;
        int a_inc = ir_new_reg(fn);
        IrOperand cpy_ai[] = {reg_op(a_next)};
        ir_emit_into_block(fn, inc, IR_COPY, a_inc, i64_type(), cpy_ai, 1, line);
        int b_inc = ir_new_reg(fn);
        IrOperand cpy_bi[] = {reg_op(b_next)};
        ir_emit_into_block(fn, inc, IR_COPY, b_inc, i64_type(), cpy_bi, 1, line);
        int next_i = ir_new_reg(fn);
        IrOperand add_ops[] = {reg_op(i_phi), reg_op(one_reg)};
        ir_emit_into_block(fn, inc, IR_ADD, next_i, i64_type(), add_ops, 2, line);
        IrOperand br_hdr3[] = {block_op(header)};
        ir_emit_into_block(fn, inc, IR_BR, -1, void_type(), br_hdr3, 1, line);

        ctx->current_block = exit_blk;
        IrOperand ret_a[] = {reg_op(a_phi)};
        ir_emit_into_block(fn, exit_blk, IR_RET, -1, void_type(), ret_a, 1, line);

        IrOperand i_args[] = {reg_op(two_reg), block_op(seed_def), reg_op(next_i), block_op(inc)};
        ir_emit_phi(fn, header, i_phi, i64_type(), i_args, 4);
        IrOperand a_args[] = {reg_op(a_seed), block_op(seed_def), reg_op(a_inc), block_op(inc)};
        ir_emit_phi(fn, header, a_phi, i64_type(), a_args, 4);
        IrOperand b_args[] = {reg_op(b_seed), block_op(seed_def), reg_op(b_inc), block_op(inc)};
        ir_emit_phi(fn, header, b_phi, i64_type(), b_args, 4);
    }

    exit_scope(ctx);
    ctx->func = &ctx->module->funcs[0];
}

bool ir_lower(AstNode **stmts, int count, IrModule *module, DiagContext *diag, const char *path) {
    LowerCtx ctx;
    ctx.module = module;
    ctx.func = NULL;
    ctx.current_block = 0;
    ctx.diag = diag;
    ctx.path = path;
    ctx.var_count = 0;
    ctx.scope_level = 0;
    ctx.loop_depth = 0;
    ctx.stream_gen_count = 0;
    ctx.enum_count = 0;

    // Discovery pass for enums and top-level stream generators.
    for (int i = 0; i < count; i++) {
        AstNode *st = stmts[i];
        if (st && st->kind == AST_ENUM_DEF && ctx.enum_count < MAX_ENUMS) {
            Token n = st->as.enum_def.name;
            char *ename = malloc(n.length + 1);
            memcpy(ename, n.start, n.length);
            ename[n.length] = '\0';
            ctx.enums[ctx.enum_count++] = (EnumEntryLower){ename, n.length, st->as.enum_def.variants, st->as.enum_def.variant_count};
        }
    }

    // M17.4.3: discovery pass for top-level stream generators. Both shapes are
    // collected before lowering so bracket call sites can resolve them.
    for (int i = 0; i < count && ctx.stream_gen_count < MAX_STREAM_GENS; i++) {
        AstNode *st = stmts[i];
        if (st && st->kind == AST_BINDING &&
            st->as.binding.expr && st->as.binding.expr->kind == AST_STREAM_GEN) {
            Token n = st->as.binding.name;
            char *sname = malloc(n.length + 1);
            memcpy(sname, n.start, n.length);
            sname[n.length] = '\0';
            ctx.stream_gens[ctx.stream_gen_count] = (StreamGenEntry){sname, n.length, st->as.binding.expr, NULL, 0};
            ctx.stream_gen_count++;
        } else if (st && st->kind == AST_FUNCTION &&
                   st->as.function.body && st->as.function.body->kind == AST_STREAM_GEN) {
            Token n = st->as.function.name;
            char *sname = malloc(n.length + 1);
            memcpy(sname, n.start, n.length);
            sname[n.length] = '\0';
            ctx.stream_gens[ctx.stream_gen_count] = (StreamGenEntry){sname, n.length, st->as.function.body,
                                                                     st->as.function.params, st->as.function.param_count};
            ctx.stream_gen_count++;
        }
    }
    // Fail closed if there are more stream generators than the limit allows.
    if (ctx.stream_gen_count >= MAX_STREAM_GENS) {
        for (int i = 0; i < count; i++) {
            AstNode *st = stmts[i];
            if ((st && st->kind == AST_BINDING &&
                 st->as.binding.expr && st->as.binding.expr->kind == AST_STREAM_GEN) ||
                (st && st->kind == AST_FUNCTION &&
                 st->as.function.body && st->as.function.body->kind == AST_STREAM_GEN)) {
                diag_error(diag, path, st->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "too many stream generators (limit 64)");
                break;
            }
        }
    }

    void *tmp_main = realloc(module->funcs, sizeof(IrFunction));
    if (!tmp_main) { diag_error(ctx.diag, ctx.path, 0, ERR_UNSUPPORTED_STATEMENT, "out of memory"); return NULL; }
    module->funcs = tmp_main;
    module->func_count = 1;
    ir_func_init(&module->funcs[0], "main", 4);
    module->funcs[0].is_main = true;
    module->funcs[0].return_type = i64_type();
    ctx.func = &module->funcs[0];

    int entry_block = ir_add_block(ctx.func, 0);
    ctx.current_block = entry_block;

    for (int i = 0; i < count; i++) {
        if (stmts[i]->kind == AST_FUNCTION &&
            !(stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN)) {
            lower_stmt(&ctx, stmts[i]);
        }
    }

    for (int i = 0; i < count; i++) {
        if (stmts[i]->kind != AST_FUNCTION &&
            !(stmts[i]->kind == AST_BINDING &&
              stmts[i]->as.binding.expr && stmts[i]->as.binding.expr->kind == AST_STREAM_GEN)) {
            lower_stmt(&ctx, stmts[i]);
        }
    }

    IrOperand ret_zero[] = {(IrOperand){IR_OP_IMM, .imm = 0}};
    int zero_reg = ir_new_reg(ctx.func);
    ir_emit_into_block(ctx.func, ctx.current_block, IR_CONST_INT, zero_reg, i64_type(), ret_zero, 1, 0);
    IrOperand ret_ops[] = {reg_op(zero_reg)};
    ir_emit_into_block(ctx.func, ctx.current_block, IR_RET, -1, void_type(), ret_ops, 1, 0);

    // Lower the stream generators after main and the regular functions so the
    // wasm start section keeps calling the main slot (FUNC_USER_BASE + 0).
    for (int g = 0; g < ctx.stream_gen_count; g++) {
        lower_stream_gen(&ctx, &ctx.stream_gens[g]);
    }

    for (int e = 0; e < ctx.enum_count; e++) {
        free(ctx.enums[e].name);
    }

    return !diag->has_error;
}
