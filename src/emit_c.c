// C11 emission backend for Tiq (split out of main.c per plan 2.1).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/lexer.h"
#include "../include/diag.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/type.h"
#include "../include/emit_c.h"
#include "../include/runtime_prelude.h"
#include "../include/runtime_aux.h"

// Emit the raw bytes of a Tiq string literal as a C string literal.
// LANGUAGE_SPEC §4 escapes (\\ \" \n \r \t \0) are spelled identically in C,
// so validated escape sequences pass through verbatim; the lexer has already
// rejected any other escape. Bare control bytes are hex-escaped.
static void emit_c_string(FILE *out, const char *start, size_t length) {
    size_t i;
    fputc('"', out);
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)start[i];
        if (ch == '\\' && i + 1 < length) {
            fputc('\\', out);
            fputc(start[++i], out);
        } else if (ch < 32U || ch == 127U) {
            fprintf(out, "\\x%02x", ch);
        } else {
            fputc((int)ch, out);
        }
    }
    fputc('"', out);
}

#define TIQ_MAX_STREAM_GENS 64
typedef struct { const char *name; Token *params; int param_count; } EmitStreamGenInfo;

// M9.2-B: one entry per scope currently being emitted, so break/skip can
// free the owned strings of every scope they exit (LANGUAGE_SPEC §16.4).
#define TIQ_MAX_SCOPE_DEPTH 64
// M9.2-F: at most this many hoisted temporaries per statement; a statement
// that overflows hoists nothing (leak, never dangle).
#define TIQ_MAX_HOIST 16
typedef struct {
    AstNode **stmts;
    int count;
    int emitted;      // index of the statement currently being emitted
    bool is_loop_body;
    // M9.2-D: the rest of the scope, scanned by the mutable-owner escape test.
    AstNode *final_expr;
    AstNode **deferred;
    int defer_count;
} EmitScope;

// All emitter state lives here; no file-static mutable globals, so the
// backend is re-entrant and unit-testable (plan 2.1).
typedef struct EmitContext {
    FILE *out;
    DiagContext *diag;
    const char *path;
    EmitStreamGenInfo stream_gens[TIQ_MAX_STREAM_GENS];
    int stream_gen_count;
    // M9.1: enclosing function during body emission; used to re-derive
    // reference parameters (semantic analysis auto-derefs their uses).
    AstNode *current_fn;
    EmitScope scopes[TIQ_MAX_SCOPE_DEPTH];
    int scope_depth;
    // M9.2-F: unbound owned temporaries hoisted out of the statement
    // currently being emitted (LANGUAGE_SPEC §16.4).
    AstNode *hoisted[TIQ_MAX_HOIST];
    int hoist_ids[TIQ_MAX_HOIST];
    int hoist_count;
    AstNode *hoist_emitting; // temp whose own initializer is being emitted
    int tmp_counter;         // monotonic across the whole translation unit
} EmitContext;

static void emit_expr(AstNode *node, EmitContext *ctx);
static void emit_stmt(AstNode *node, EmitContext *ctx, int indent);
static void emit_type_name(PrimitiveType kind, FILE *out);

// M9.2: an immutable binding whose initializer is a direct call to one of
// these builtins owns the returned heap string (LANGUAGE_SPEC §16.4).
static bool is_owned_str_builtin_call(AstNode *expr) {
    static const struct { const char *name; int len; } owned[] = {
        {"fs_read", 7}, {"json_encode_str", 15}, {"json_get", 8},
        {"json_arr_get", 12}, {"net_fetch", 9}, {"net_recv", 8},
        {"http_method", 11}, {"http_path", 9}, {"json_set", 8}, {"json_del", 8},
        {"str_cat", 7}, {"int_str", 7}, {"http_header", 11},
    };
    if (!expr || expr->kind != AST_CALL || expr->as.call.is_bracket_call) return false;
    if (!expr->as.call.callee || expr->as.call.callee->kind != AST_IDENTIFIER) return false;
    Token n = expr->as.call.callee->as.identifier.name;
    for (int i = 0; i < (int)(sizeof owned / sizeof owned[0]); i++) {
        if ((int)n.length == owned[i].len &&
            memcmp(n.start, owned[i].name, n.length) == 0)
            return true;
    }
    return false;
}

// M9.2-I: user functions classified fresh-result (LANGUAGE_SPEC §16.4);
// filled once per program before emission. Classification never consults
// this table, so it is order-independent.
#define TIQ_MAX_FRESH_FNS 64
static Token fresh_str_fns[TIQ_MAX_FRESH_FNS];
static int fresh_str_fn_count;

// M9.2-I: a direct call to a fresh-result function returns heap storage the
// caller cannot already reach, so the binding it initializes owns it.
static bool is_fresh_str_fn_call(AstNode *expr) {
    if (!expr || expr->kind != AST_CALL || expr->as.call.is_bracket_call) return false;
    if (!expr->as.call.callee || expr->as.call.callee->kind != AST_IDENTIFIER) return false;
    Token n = expr->as.call.callee->as.identifier.name;
    for (int i = 0; i < fresh_str_fn_count; i++) {
        if (fresh_str_fns[i].length == n.length &&
            memcmp(fresh_str_fns[i].start, n.start, n.length) == 0)
            return true;
    }
    return false;
}

// M9.2: initializers that make their binding an owner (§16.4).
static bool is_owning_str_init(AstNode *expr);

// M9.2-K: a conditional expression is an owning expression when both
// branches are owning expressions (recursive through nested conditionals).
static bool is_owning_conditional(AstNode *expr) {
    if (!expr || expr->kind != AST_CONDITIONAL) return false;
    return is_owning_str_init(expr->as.conditional.then_branch) &&
           is_owning_str_init(expr->as.conditional.else_branch);
}

// M9.2: initializers that make their binding an owner (§16.4).
static bool is_owning_str_init(AstNode *expr) {
    return is_owned_str_builtin_call(expr) || is_fresh_str_fn_call(expr) ||
           is_owning_conditional(expr);
}

// M9.2: does statement s bind an owned string? If so, report its name.
static bool owned_binding_name(AstNode *s, Token *name) {
    if (s && s->kind == AST_BINDING && !s->as.binding.is_mutable &&
        is_owning_str_init(s->as.binding.expr)) {
        *name = s->as.binding.name;
        return true;
    }
    if (s && s->kind == AST_ASSIGN && s->as.assign.is_definition &&
        s->as.assign.op == TOK_EQ && !s->as.assign.index &&
        is_owning_str_init(s->as.assign.expr)) {
        *name = s->as.assign.name;
        return true;
    }
    return false;
}

// M9.2-D: does statement s declare a mutable binding whose initializer is
// an owned builtin call? Ownership also needs the escape test below.
static bool mut_owned_decl(AstNode *s, Token *name) {
    if (s && s->kind == AST_BINDING && s->as.binding.is_mutable &&
        is_owning_str_init(s->as.binding.expr)) {
        *name = s->as.binding.name;
        return true;
    }
    if (s && s->kind == AST_ASSIGN && s->as.assign.is_definition &&
        s->as.assign.op == TOK_LARROW && !s->as.assign.index &&
        is_owning_str_init(s->as.assign.expr)) {
        *name = s->as.assign.name;
        return true;
    }
    return false;
}

static bool tok_name_eq(Token a, Token b) {
    return a.length == b.length && memcmp(a.start, b.start, a.length) == 0;
}

// M9.2-D: standard-library builtins that read a string argument without
// retaining it; a mutable owner's name may appear only in these positions.
static bool is_safe_builtin_callee(AstNode *callee) {
    static const struct { const char *name; int len; } safe[] = {
        {"print", 5}, {"len", 3},
        {"fs_read", 7}, {"fs_write", 8}, {"fs_exists", 9},
        {"proc_exec", 9}, {"proc_exit", 9},
        {"json_parse_int", 14}, {"json_encode_str", 15}, {"json_get", 8},
        {"json_arr_len", 12}, {"json_arr_get", 12}, {"net_fetch", 9},
        {"cli_arg_count", 13}, {"cli_arg", 7},
        {"net_listen", 10}, {"net_accept", 10}, {"net_connect", 11},
        {"net_recv", 8}, {"net_send", 8}, {"net_close", 9},
        {"net_port", 8}, {"net_shutdown", 12},
        {"http_method", 11}, {"http_path", 9},
        {"ev_loop", 7}, {"ev_add", 6}, {"ev_wait", 7}, {"ev_ready", 8},
        {"json_set", 8}, {"json_del", 8}, {"str_cat", 7}, {"int_str", 7},
        {"http_header", 11},
    };
    if (!callee || callee->kind != AST_IDENTIFIER) return false;
    Token n = callee->as.identifier.name;
    for (int i = 0; i < (int)(sizeof safe / sizeof safe[0]); i++)
        if ((int)n.length == safe[i].len &&
            memcmp(n.start, safe[i].name, n.length) == 0)
            return true;
    return false;
}

// M9.2-D conservative escape test (LANGUAGE_SPEC §16.4): every use of a
// mutable owner's name must be an argument to a standard-library builtin,
// and every `<-` assignment to it must be a direct owned-builtin call.
// Unknown constructs fail closed: the binding leaks, it never dangles.
static bool mut_uses_ok(AstNode *n, Token name);

static bool mut_call_args_ok(AstNode *call, Token name) {
    for (int i = 0; i < call->as.call.arg_count; i++) {
        AstNode *a = call->as.call.args[i];
        if (a && a->kind == AST_IDENTIFIER &&
            tok_name_eq(a->as.identifier.name, name)) continue;
        if (!mut_uses_ok(a, name)) return false;
    }
    return true;
}

static bool mut_uses_ok(AstNode *n, Token name) {
    if (!n) return true;
    switch (n->kind) {
        case AST_LITERAL: case AST_BREAK: case AST_SKIP:
        case AST_STRUCT_DEF: case AST_CHAN:
            return true;
        case AST_IDENTIFIER:
            return !tok_name_eq(n->as.identifier.name, name);
        case AST_BINARY:
            return mut_uses_ok(n->as.binary.left, name) &&
                   mut_uses_ok(n->as.binary.right, name);
        case AST_UNARY:
            return mut_uses_ok(n->as.unary.right, name);
        case AST_CONDITIONAL:
            return mut_uses_ok(n->as.conditional.cond, name) &&
                   mut_uses_ok(n->as.conditional.then_branch, name) &&
                   mut_uses_ok(n->as.conditional.else_branch, name);
        case AST_CALL:
            if (!n->as.call.is_bracket_call &&
                is_safe_builtin_callee(n->as.call.callee))
                return mut_call_args_ok(n, name);
            if (!mut_uses_ok(n->as.call.callee, name)) return false;
            for (int i = 0; i < n->as.call.arg_count; i++)
                if (!mut_uses_ok(n->as.call.args[i], name)) return false;
            return true;
        case AST_BLOCK:
            for (int i = 0; i < n->as.block.stmt_count; i++)
                if (!mut_uses_ok(n->as.block.statements[i], name)) return false;
            for (int i = 0; i < n->as.block.defer_count; i++)
                if (!mut_uses_ok(n->as.block.deferred[i], name)) return false;
            return mut_uses_ok(n->as.block.final_expr, name);
        case AST_BINDING:
            // A same-named (re)binding shadows or aliases: fail closed.
            if (tok_name_eq(n->as.binding.name, name)) return false;
            return mut_uses_ok(n->as.binding.expr, name);
        case AST_ASSIGN:
            if (tok_name_eq(n->as.assign.name, name)) {
                if (n->as.assign.is_definition) return false;
                if (n->as.assign.op != TOK_LARROW || n->as.assign.index) return false;
                if (!is_owned_str_builtin_call(n->as.assign.expr)) return false;
                return mut_call_args_ok(n->as.assign.expr, name);
            }
            return mut_uses_ok(n->as.assign.index, name) &&
                   mut_uses_ok(n->as.assign.expr, name);
        case AST_FUNCTION:
            return mut_uses_ok(n->as.function.body, name);
        case AST_BRACKET_LOOP: {
            // The binder (explicit, or the implicit range index `i`)
            // shadows the owner inside the body: fail closed.
            bool shadows = n->as.bracket_loop.has_binder ?
                tok_name_eq(n->as.bracket_loop.binder, name) :
                (name.length == 1 && name.start[0] == 'i');
            if (shadows) return false;
            if (!mut_uses_ok(n->as.bracket_loop.domain, name)) return false;
            for (int i = 0; i < n->as.bracket_loop.body_count; i++)
                if (!mut_uses_ok(n->as.bracket_loop.body_stmts[i], name)) return false;
            return mut_uses_ok(n->as.bracket_loop.body_final, name);
        }
        case AST_STREAM_GEN:
            for (int i = 0; i < n->as.stream_gen.seed_count; i++)
                if (!mut_uses_ok(n->as.stream_gen.seeds[i], name)) return false;
            return mut_uses_ok(n->as.stream_gen.gen_expr, name) &&
                   mut_uses_ok(n->as.stream_gen.bound, name);
        case AST_ARRAY:
            for (int i = 0; i < n->as.array.element_count; i++)
                if (!mut_uses_ok(n->as.array.elements[i], name)) return false;
            return true;
        case AST_ARRAY_FILL:
            return mut_uses_ok(n->as.array_fill.value, name) &&
                   mut_uses_ok(n->as.array_fill.length, name);
        case AST_FIELD_ACCESS:
            return mut_uses_ok(n->as.field_access.target, name);
        case AST_RECORD_LIT:
            for (int i = 0; i < n->as.record_lit.field_count; i++)
                if (!mut_uses_ok(n->as.record_lit.field_values[i], name)) return false;
            return true;
        case AST_MATCH:
            if (!mut_uses_ok(n->as.match_expr.expr, name)) return false;
            for (int i = 0; i < n->as.match_expr.arm_count; i++) {
                if (!mut_uses_ok(n->as.match_expr.arms[i].pattern, name)) return false;
                if (!mut_uses_ok(n->as.match_expr.arms[i].body, name)) return false;
            }
            return true;
        case AST_SPAWN:
            return mut_uses_ok(n->as.spawn.expr, name);
        case AST_DEFER:
            return mut_uses_ok(n->as.defer.expr, name);
        default:
            return false; // fail closed on unknown constructs
    }
}

// M9.2-D: full qualification for a mutable owner declared at decl_idx of sc.
static bool mut_owner_qualifies(const EmitScope *sc, int decl_idx, Token name) {
    for (int i = decl_idx + 1; i < sc->count; i++)
        if (!mut_uses_ok(sc->stmts[i], name)) return false;
    if (!mut_uses_ok(sc->final_expr, name)) return false;
    for (int i = 0; i < sc->defer_count; i++)
        if (!mut_uses_ok(sc->deferred[i], name)) return false;
    return true;
}

// M9.2: is scope statement i an owner (immutable, or qualifying mutable)?
static bool owned_name_at(const EmitScope *sc, int i, Token *name) {
    if (owned_binding_name(sc->stmts[i], name)) return true;
    return mut_owned_decl(sc->stmts[i], name) &&
           mut_owner_qualifies(sc, i, *name);
}

// M9.2: free owned strings declared among the first `limit` stmts, in
// reverse declaration order. Runs at scope end (limit = full statement
// count, after that scope's defers) and on break/skip (limit = index of
// the jump statement, so only owners already bound are freed).
// M9.2-H: skip_idx names an owner statement excluded from destruction
// (its string transfers to the caller); pass -1 to free every owner.
static void emit_scope_frees_except(const EmitScope *sc, int limit, int skip_idx,
                                    EmitContext *ctx, int indent) {
    for (int i = limit - 1; i >= 0; i--) {
        Token name;
        if (i == skip_idx) continue;
        if (!owned_name_at(sc, i, &name)) continue;
        for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
        fprintf(ctx->out, "free((void *)%.*s);\n", (int)name.length, name.start);
    }
}

static void emit_scope_frees(const EmitScope *sc, int limit, EmitContext *ctx, int indent) {
    emit_scope_frees_except(sc, limit, -1, ctx, indent);
}

static bool scope_has_owned(const EmitScope *sc) {
    Token name;
    for (int i = 0; i < sc->count; i++)
        if (owned_name_at(sc, i, &name)) return true;
    return false;
}

// M9.2-H: does `want` name an owner of sc? Report its statement index.
static bool scope_owner_index(const EmitScope *sc, Token want, int *idx) {
    Token name;
    for (int i = 0; i < sc->count; i++) {
        if (!owned_name_at(sc, i, &name)) continue;
        if (tok_name_eq(name, want)) { *idx = i; return true; }
    }
    return false;
}

// M9.2-I: is `fn` fresh-result (LANGUAGE_SPEC §16.4)? Its `str` result must
// be a direct owned-builtin call or a bare identifier naming an owner of the
// body's outermost scope; both are heap storage the caller cannot reach.
static bool fn_is_fresh_result(AstNode *fn) {
    if (!fn || fn->kind != AST_FUNCTION || !fn->as.function.body) return false;
    AstNode *body = fn->as.function.body;
    AstNode *res = (body->kind == AST_BLOCK) ? body->as.block.final_expr : body;
    if (!res) return false;
    SemanticType *t = fn->semantic_type;
    SemanticType *rt = (t && t->kind != TYPE_UNKNOWN) ? t : res->semantic_type;
    if (!rt || rt->kind != TYPE_STR) return false;
    if (is_owned_str_builtin_call(res)) return true;
    if (res->kind == AST_IDENTIFIER && body->kind == AST_BLOCK) {
        EmitScope sc = { body->as.block.statements,
                         body->as.block.stmt_count,
                         body->as.block.stmt_count, false,
                         body->as.block.final_expr,
                         body->as.block.deferred,
                         body->as.block.defer_count };
        int idx;
        return scope_owner_index(&sc, res->as.identifier.name, &idx);
    }
    return false;
}

// M9.2-I: classify every top-level function once, before emission. The
// classifier never reads the table (it is still empty here), so the result
// does not depend on declaration order.
static void collect_fresh_str_fns(AstNode **stmts, int count) {
    Token found[TIQ_MAX_FRESH_FNS];
    int n = 0;
    fresh_str_fn_count = 0;
    for (int i = 0; i < count && n < TIQ_MAX_FRESH_FNS; i++) {
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION &&
            fn_is_fresh_result(stmts[i]))
            found[n++] = stmts[i]->as.function.name;
    }
    for (int i = 0; i < n; i++) fresh_str_fns[i] = found[i];
    fresh_str_fn_count = n;
}

// M9.2-C: a function may destroy its owners only when its result type
// cannot carry a pointer into one of them (LANGUAGE_SPEC §16.4).
static bool is_scalar_result(SemanticType *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_INT: case TYPE_FLOAT: case TYPE_BOOL:
        case TYPE_I8: case TYPE_I16: case TYPE_I32:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
        case TYPE_F32:
            return true;
        default:
            return false;
    }
}

// M9.2-B: push/pop the scope currently being emitted. Depth overflow fails
// safe: the scope is not tracked, so its owners leak on break/skip but are
// never double-freed.
static void emit_scope_push(EmitContext *ctx, AstNode **stmts, int count, bool is_loop_body,
                            AstNode *final_expr, AstNode **deferred, int defer_count) {
    if (ctx->scope_depth >= TIQ_MAX_SCOPE_DEPTH) { ctx->scope_depth++; return; }
    EmitScope *s = &ctx->scopes[ctx->scope_depth++];
    s->stmts = stmts; s->count = count; s->emitted = 0; s->is_loop_body = is_loop_body;
    s->final_expr = final_expr; s->deferred = deferred; s->defer_count = defer_count;
}

static void emit_scope_pop(EmitContext *ctx) {
    if (ctx->scope_depth > 0) ctx->scope_depth--;
}

// M9.2-B: before a break or skip transfers control, free the owned strings
// of every scope it exits: innermost first, through the enclosing loop body.
static void emit_jump_frees(EmitContext *ctx, int indent) {
    // Untracked inner scopes (depth overflow) would make outer frees unsafe.
    if (ctx->scope_depth > TIQ_MAX_SCOPE_DEPTH) return;
    for (int d = ctx->scope_depth - 1; d >= 0; d--) {
        EmitScope *s = &ctx->scopes[d];
        emit_scope_frees(s, s->emitted, ctx, indent);
        if (s->is_loop_body) break;
    }
}

// M9.2-D: does `name <- ...` reassign a qualifying mutable owner? Search
// the scope stack innermost-first for the declaring statement.
static bool mut_reassign_owner(EmitContext *ctx, Token name) {
    // Untracked inner scopes (depth overflow) may shadow the name.
    if (ctx->scope_depth > TIQ_MAX_SCOPE_DEPTH) return false;
    for (int d = ctx->scope_depth - 1; d >= 0; d--) {
        const EmitScope *sc = &ctx->scopes[d];
        for (int i = 0; i < sc->count; i++) {
            AstNode *s = sc->stmts[i];
            bool declares =
                (s && s->kind == AST_BINDING &&
                 tok_name_eq(s->as.binding.name, name)) ||
                (s && s->kind == AST_ASSIGN && s->as.assign.is_definition &&
                 tok_name_eq(s->as.assign.name, name));
            if (!declares) continue;
            Token n2;
            return mut_owned_decl(s, &n2) && mut_owner_qualifies(sc, i, name);
        }
    }
    return false;
}

// M9.2-E: before a statement-level proc_exit terminates the process, free
// the owned strings of every tracked scope, innermost first (§16.4).
static void emit_exit_frees(EmitContext *ctx, int indent) {
    // Untracked inner scopes (depth overflow) would make the frees unsafe.
    if (ctx->scope_depth > TIQ_MAX_SCOPE_DEPTH) return;
    for (int d = ctx->scope_depth - 1; d >= 0; d--) {
        EmitScope *s = &ctx->scopes[d];
        emit_scope_frees(s, s->emitted, ctx, indent);
    }
}

static bool is_proc_exit_call(AstNode *node) {
    return node && node->kind == AST_CALL && !node->as.call.is_bracket_call &&
           node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
           node->as.call.callee->as.identifier.name.length == 9 &&
           memcmp(node->as.call.callee->as.identifier.name.start, "proc_exit", 9) == 0 &&
           node->as.call.arg_count == 1;
}

// M9.2-F: collect the unbound owned temporaries of a simple statement that
// sit in unconditionally evaluated positions: the bare statement expression
// itself, or a direct argument to a standard-library builtin, nested to any
// depth through such builtins (§16.4). M9.2-J extends both roles to calls to
// fresh-result functions: such a call returns storage the caller cannot
// already reach, so it is itself a temporary, and its arguments are
// unconditionally evaluated positions the result cannot alias. M9.2-L extends
// hoisting to conditional expressions whose branches are both owning
// expressions: exactly one branch is evaluated, and every branch produces
// distinct fresh heap storage, so the conditional as a whole is a safe
// temporary. Post-order, so temporaries evaluate left to right, inner before
// outer. Any other position (match arms, non-fresh-result calls, conditionals
// with non-owning branches) is skipped: those temporaries leak, never dangle.
static void hoist_collect(AstNode *n, bool root_bound, EmitContext *ctx, bool *overflow) {
    if (!n) return;
    // M9.2-L: a conditional whose branches are both owning expressions is
    // itself a temporary when unbound; hoist the whole conditional (exactly
    // one branch evaluates at runtime, both produce fresh heap storage).
    if (n->kind == AST_CONDITIONAL) {
        if (!root_bound && is_owning_conditional(n)) {
            if (ctx->hoist_count >= TIQ_MAX_HOIST) { *overflow = true; return; }
            ctx->hoisted[ctx->hoist_count] = n;
            ctx->hoist_ids[ctx->hoist_count] = ctx->tmp_counter++;
            ctx->hoist_count++;
        }
        return;
    }
    if (n->kind != AST_CALL || n->as.call.is_bracket_call) return;
    if (!n->as.call.callee || n->as.call.callee->kind != AST_IDENTIFIER) return;
    if (!is_safe_builtin_callee(n->as.call.callee) && !is_fresh_str_fn_call(n)) return;
    for (int i = 0; i < n->as.call.arg_count; i++)
        hoist_collect(n->as.call.args[i], false, ctx, overflow);
    if (!root_bound && is_owning_str_init(n)) {
        if (ctx->hoist_count >= TIQ_MAX_HOIST) { *overflow = true; return; }
        ctx->hoisted[ctx->hoist_count] = n;
        ctx->hoist_ids[ctx->hoist_count] = ctx->tmp_counter++;
        ctx->hoist_count++;
    }
}

// M9.1: 0 = not a reference parameter of the enclosing function,
// 1 = shared borrow (&T), 2 = mutable borrow (&mut T).
static int ref_param_kind(EmitContext *ctx, Token name) {
    AstNode *fn = ctx->current_fn;
    if (!fn) return 0;
    for (int i = 0; i < fn->as.function.param_count; i++) {
        if (fn->as.function.params[i].length == name.length &&
            memcmp(fn->as.function.params[i].start, name.start, name.length) == 0) {
            SemanticType *pt = fn->as.function.param_types ?
                (SemanticType *)fn->as.function.param_types[i] : NULL;
            if (pt && pt->kind == TYPE_REF) return 1;
            if (pt && pt->kind == TYPE_REF_MUT) return 2;
            return 0;
        }
    }
    return 0;
}

static bool is_stream_gen_name(EmitContext *ctx, const char *name, int len, int *out_params, int *out_param_count) {
    for (int i = 0; i < ctx->stream_gen_count; i++) {
        if ((int)strlen(ctx->stream_gens[i].name) == len &&
            memcmp(ctx->stream_gens[i].name, name, len) == 0) {
            *out_params = 0;
            *out_param_count = ctx->stream_gens[i].param_count;
            return true;
        }
    }
    return false;
}

static const char *binary_op_c_str(TokenKind op) {
    switch (op) {
        case TOK_PLUS: return "+"; case TOK_MINUS: return "-"; case TOK_STAR: return "*";
        case TOK_SLASH: return "/"; case TOK_PERCENT: return "%";
        case TOK_EQ_EQ: return "=="; case TOK_BANG_EQ: return "!=";
        case TOK_LT: return "<"; case TOK_LTE: return "<="; case TOK_GT: return ">"; case TOK_GTE: return ">=";
        case TOK_AND_AND: return "&&"; case TOK_OR_OR: return "||";
        case TOK_AMP: return "&"; case TOK_PIPE: return "|"; case TOK_CARET: return "^";
        case TOK_LSHIFT: return "<<"; case TOK_RSHIFT: return ">>";
        default: return "?";
    }
}

static void emit_expr(AstNode *node, EmitContext *ctx) {
    if (!node) return;
    // M9.2-F: a hoisted temporary reads from its hidden binding (§16.4).
    if (node != ctx->hoist_emitting) {
        for (int i = 0; i < ctx->hoist_count; i++) {
            if (ctx->hoisted[i] == node) {
                fprintf(ctx->out, "tiq_tmp%d", ctx->hoist_ids[i]);
                return;
            }
        }
    }
    switch (node->kind) {
        case AST_LITERAL: {
            if (node->as.literal.type == TOK_INT)
                // Integer literals are i64 (LANGUAGE_SPEC §11); suffix keeps
                // C constant arithmetic in 64-bit before any conversion.
                fprintf(ctx->out, "%.*sLL", (int)node->token.length, node->token.start);
            else if (node->as.literal.type == TOK_FLOAT)
                fprintf(ctx->out, "%.*s", (int)node->token.length, node->token.start);
            else if (node->as.literal.type == TOK_STRING) emit_c_string(ctx->out, node->token.start + 1, node->token.length - 2);
            else if (node->as.literal.type == TOK_TRUE) fputs("1", ctx->out);
            else if (node->as.literal.type == TOK_FALSE) fputs("0", ctx->out);
            break;
        }
        case AST_IDENTIFIER:
            // M8: 'none' is a polymorphic Option constructor keyword.
            if (node->as.identifier.name.length == 4 &&
                memcmp(node->as.identifier.name.start, "none", 4) == 0) {
                fputs("((TiqOption){ .value = 0, .has_value = 0 })", ctx->out);
            } else if (ref_param_kind(ctx, node->as.identifier.name) != 0) {
                // M9.1: reference parameters deref to the referent.
                fprintf(ctx->out, "(*%.*s)", (int)node->as.identifier.name.length, node->as.identifier.name.start);
            } else {
                fprintf(ctx->out, "%.*s", (int)node->as.identifier.name.length, node->as.identifier.name.start);
            }
            break;
        case AST_BINARY: {
            // M8: Fallback operator ?? for Option/Result types.
            if (node->as.binary.op == TOK_QUESTION_QUESTION) {
                SemanticType *lt = node->as.binary.left ? node->as.binary.left->semantic_type : NULL;
                const char *flag_field = (lt && lt->kind == TYPE_RESULT) ? ".is_ok" : ".has_value";
                fputs("(", ctx->out);
                emit_expr(node->as.binary.left, ctx);
                fputs(flag_field, ctx->out);
                fputs(" ? ", ctx->out);
                emit_expr(node->as.binary.left, ctx);
                fputs(".value : ", ctx->out);
                emit_expr(node->as.binary.right, ctx);
                fputs(")", ctx->out);
                break;
            }
            fputs("(", ctx->out);
            emit_expr(node->as.binary.left, ctx);
            fputs(" ", ctx->out);
            fputs(binary_op_c_str(node->as.binary.op), ctx->out);
            fputs(" ", ctx->out);
            emit_expr(node->as.binary.right, ctx);
            fputs(")", ctx->out);
            break;
        }
        case AST_UNARY: {
            if (node->as.unary.op == TOK_MOVE) {
                emit_expr(node->as.unary.right, ctx);
            } else if (node->as.unary.op == TOK_QUESTION) {
                // M8: Propagation operator - unwrap Option/Result value.
                // Full early-return semantics not yet implemented; emit .value access.
                emit_expr(node->as.unary.right, ctx);
                fputs(".value", ctx->out);
            } else if (node->as.unary.op == TOK_AMP) {
                // M9.1: borrow argument; semantic analysis guarantees the
                // operand is a plain named binding in the caller's scope.
                fputs("&", ctx->out);
                emit_expr(node->as.unary.right, ctx);
            } else {
                const char *op = "";
                if (node->as.unary.op == TOK_BANG) op = "!";
                else if (node->as.unary.op == TOK_MINUS) op = "-";
                else if (node->as.unary.op == TOK_PLUS) op = "+";
                fputs(op, ctx->out); fputs("(", ctx->out);
                emit_expr(node->as.unary.right, ctx);
                fputs(")", ctx->out);
            }
            break;
        }
        case AST_CONDITIONAL:
            emit_expr(node->as.conditional.cond, ctx);
            fputs(" ? ", ctx->out);
            emit_expr(node->as.conditional.then_branch, ctx);
            fputs(" : ", ctx->out);
            emit_expr(node->as.conditional.else_branch, ctx);
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 5 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "print", 5) == 0 &&
                node->as.call.arg_count == 1) {
                // print builtin: one printf call per printable type; the
                // expression value is printf's return (bytes written).
                AstNode *arg = node->as.call.args[0];
                SemanticType *st = arg ? arg->semantic_type : NULL;
                PrimitiveType kind = st ? st->kind : TYPE_INT;
                if (kind == TYPE_STR) {
                    fputs("printf(\"%s\\n\", ", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(")", ctx->out);
                } else if (kind == TYPE_FLOAT || kind == TYPE_F32) {
                    fputs("printf(\"%g\\n\", (double)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs("))", ctx->out);
                } else if (kind == TYPE_BOOL) {
                    fputs("printf(\"%s\\n\", (", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(") ? \"true\" : \"false\")", ctx->out);
                } else if (kind == TYPE_STR_VIEW || kind == TYPE_SLICE) {
                    fputs("printf(\"%.*s\\n\", ((TiqSlice)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(")).len, (const char*)(((TiqSlice)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs(")).ptr))", ctx->out);
                } else {
                    fputs("printf(\"%lld\\n\", (long long)(", ctx->out);
                    emit_expr(arg, ctx);
                    fputs("))", ctx->out);
                }
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 3 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "len", 3) == 0) {
                SemanticType *ct = node->as.call.args[0] ?
                    node->as.call.args[0]->semantic_type : NULL;
                if (ct && ct->kind == TYPE_ARRAY) {
                    fprintf(ctx->out, "%d", ct->array_length);
                } else if (ct && (ct->kind == TYPE_SLICE || ct->kind == TYPE_STR_VIEW)) {
                    fputs("(((TiqSlice)", ctx->out);
                    emit_expr(node->as.call.args[0], ctx);
                    fputs(").len)", ctx->out);
                } else if (ct && ct->kind == TYPE_STR) {
                    fputs("((int)strlen(", ctx->out);
                    emit_expr(node->as.call.args[0], ctx);
                    fputs("))", ctx->out);
                } else {
                    fprintf(ctx->out, "0");
                }
                break;
            }
            // M8: Option constructors some(x) and none.
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 4 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "some", 4) == 0 &&
                node->as.call.arg_count == 1) {
                fputs("((TiqOption){ .value = (int64_t)(", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs("), .has_value = 1 })", ctx->out);
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 4 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "none", 4) == 0 &&
                node->as.call.arg_count == 0) {
                fputs("((TiqOption){ .value = 0, .has_value = 0 })", ctx->out);
                break;
            }
            // M8: Result constructors ok(x) and err(e).
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 2 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "ok", 2) == 0 &&
                node->as.call.arg_count == 1) {
                fputs("((TiqResult){ .value = (int64_t)(", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs("), .error = 0, .is_ok = 1 })", ctx->out);
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER &&
                node->as.call.callee->as.identifier.name.length == 3 &&
                memcmp(node->as.call.callee->as.identifier.name.start, "err", 3) == 0 &&
                node->as.call.arg_count == 1) {
                fputs("((TiqResult){ .value = 0, .error = (int64_t)(", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs("), .is_ok = 0 })", ctx->out);
                break;
            }
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                typedef struct { const char *tiq; int len; const char *c; } Btn;
                static const Btn btn[] = {
                    {"fs_read", 7, "tiq_fs_read"}, {"fs_write", 8, "tiq_fs_write"},
                    {"fs_exists", 9, "tiq_fs_exists"}, {"proc_exec", 9, "tiq_proc_exec"},
                    {"proc_exit", 9, "tiq_proc_exit"}, {"json_parse_int", 14, "tiq_json_parse_int"},
                    {"json_encode_str", 15, "tiq_json_encode_str"}, {"net_fetch", 9, "tiq_net_fetch"},
                    {"cli_arg_count", 13, "tiq_cli_arg_count"}, {"cli_arg", 7, "tiq_cli_arg"},
                    {"json_get", 8, "tiq_json_get"},
                    {"json_arr_len", 12, "tiq_json_arr_len"}, {"json_arr_get", 12, "tiq_json_arr_get"},
                    {"json_view", 9, "tiq_json_view"},
                    {"json_has", 8, "tiq_json_has"},
                    {"net_listen", 10, "tiq_net_listen"}, {"net_accept", 10, "tiq_net_accept"},
                    {"net_connect", 11, "tiq_net_connect"}, {"net_recv", 8, "tiq_net_recv"},
                    {"net_send", 8, "tiq_net_send"}, {"net_close", 9, "tiq_net_close"},
                    {"net_port", 8, "tiq_net_port"}, {"net_shutdown", 12, "tiq_net_shutdown"},
                    {"http_method", 11, "tiq_http_method"}, {"http_path", 9, "tiq_http_path"},
                    {"ev_loop", 7, "tiq_ev_loop"}, {"ev_add", 6, "tiq_ev_add"},
                    {"ev_wait", 7, "tiq_ev_wait"}, {"ev_ready", 8, "tiq_ev_ready"},
                    {"json_set", 8, "tiq_json_set"}, {"json_del", 8, "tiq_json_del"},
                    {"str_cat", 7, "tiq_str_cat"}, {"int_str", 7, "tiq_int_str"},
                    {"http_header", 11, "tiq_http_header"},
                };
                const char *builtin_fn = NULL;
                for (int bi = 0; bi < (int)(sizeof btn / sizeof btn[0]); bi++) {
                    if ((int)name.length == btn[bi].len && memcmp(name.start, btn[bi].tiq, name.length) == 0) {
                        builtin_fn = btn[bi].c;
                        break;
                    }
                }
                if (builtin_fn) {
                    fprintf(ctx->out, "%s(", builtin_fn);
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        if (i > 0) fputs(", ", ctx->out);
                        if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                    }
                    fputs(")", ctx->out);
                    break;
                }
                // M12.3: Explicit numeric type conversion.
                // The semantic checker already resolved node->semantic_type to the
                // target type. Detect by matching the callee name against the
                // conversion table and checking the result is a scalar primitive.
                // Emit: ((C_type)(arg_expr))
                {
                    typedef struct { const char *n; int len; } CnvName;
                    static const CnvName cnv[] = {
                        {"i8",2},{"i16",3},{"i32",3},{"i64",3},
                        {"u8",2},{"u16",3},{"u32",3},{"u64",3},
                        {"f32",3},{"f64",3},{"bool",4},{"str",3},
                    };
                    int is_conv = 0;
                    for (int k = 0; k < (int)(sizeof cnv/sizeof cnv[0]); k++) {
                        if ((int)name.length == cnv[k].len &&
                            memcmp(name.start, cnv[k].n, (size_t)cnv[k].len) == 0) {
                            is_conv = 1; break;
                        }
                    }
                    SemanticType *rst = node->semantic_type;
                    // Only emit the cast if the result is a scalar numeric/bool type
                    // (not array/slice/unknown/struct). Unknown means an error was
                    // already reported; fall through to avoid double output.
                    bool result_scalar = rst && (
                        rst->kind == TYPE_INT   || rst->kind == TYPE_FLOAT ||
                        rst->kind == TYPE_BOOL  || rst->kind == TYPE_I8    ||
                        rst->kind == TYPE_I16   || rst->kind == TYPE_I32   ||
                        rst->kind == TYPE_U8    || rst->kind == TYPE_U16   ||
                        rst->kind == TYPE_U32   || rst->kind == TYPE_U64   ||
                        rst->kind == TYPE_F32);
                    if (is_conv && result_scalar && node->as.call.arg_count == 1 &&
                        node->as.call.args[0]) {
                        fputs("((", ctx->out);
                        emit_type_name(rst->kind, ctx->out);
                        fputs(")(", ctx->out);
                        emit_expr(node->as.call.args[0], ctx);
                        fputs("))", ctx->out);
                        break;
                    }
                }
            }

            if (node->as.call.is_bracket_call && node->as.call.is_slice && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                fputs("((TiqSlice){ .ptr = ", ctx->out);
                if (ct && (ct->kind == TYPE_SLICE || ct->kind == TYPE_STR_VIEW)) {
                    fputs("((const char*)(((TiqSlice)", ctx->out);
                    emit_expr(node->as.call.callee, ctx);
                    fputs(").ptr) + (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") * ", ctx->out);
                    if (ct->kind == TYPE_SLICE) fputs("sizeof(int64_t)", ctx->out);
                    else fputs("1", ctx->out);
                    fputs("), .len = (", ctx->out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], ctx);
                    else {
                        fputs("(((TiqSlice)", ctx->out);
                        emit_expr(node->as.call.callee, ctx);
                        fputs(").len)", ctx->out);
                    }
                    fputs(") - (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") })", ctx->out);
                } else if (ct && ct->kind == TYPE_STR) {
                    fputs("((const char*)(", ctx->out);
                    emit_expr(node->as.call.callee, ctx);
                    fputs(") + (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(")), .len = (", ctx->out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], ctx);
                    else {
                        fputs("((int)strlen(", ctx->out);
                        emit_expr(node->as.call.callee, ctx);
                        fputs("))", ctx->out);
                    }
                    fputs(") - (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") })", ctx->out);
                } else {
                    int arr_len = ct ? ct->array_length : 0;
                    fputs("((const char*)(", ctx->out);
                    emit_expr(node->as.call.callee, ctx);
                    fputs(") + (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") * sizeof(int64_t)), .len = (", ctx->out);
                    if (node->as.call.args[1]) emit_expr(node->as.call.args[1], ctx);
                    else fprintf(ctx->out, "%d", arr_len);
                    fputs(") - (", ctx->out);
                    if (node->as.call.args[0]) emit_expr(node->as.call.args[0], ctx);
                    else fputs("0", ctx->out);
                    fputs(") })", ctx->out);
                }
                break;
            }
            if (node->as.call.is_bracket_call && node->as.call.callee) {
                SemanticType *ct = node->as.call.callee->semantic_type;
                if (ct && (ct->kind == TYPE_ARRAY || ct->kind == TYPE_SLICE)) {
                    if (ct->kind == TYPE_ARRAY) {
                        int len = ct->array_length;
                        if (len > 0 && node->as.call.arg_count > 0 && node->as.call.args[0]) {
                            fputs("((uint64_t)(", ctx->out);
                            emit_expr(node->as.call.args[0], ctx);
                            fprintf(ctx->out, ") < (uint64_t)(%d) ? ", len);
                            emit_expr(node->as.call.callee, ctx);
                            fputs("[", ctx->out);
                            for (int i = 0; i < node->as.call.arg_count; i++) {
                                if (i > 0) fputs("][", ctx->out);
                                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                            }
                            fputs("] : (fprintf(stderr, \"tiq: index %lld out of bounds for array of length %d\\n\", (long long)(", ctx->out);
                            emit_expr(node->as.call.args[0], ctx);
                            fprintf(ctx->out, "), %d), exit(1), ", len);
                            emit_expr(node->as.call.callee, ctx);
                            fputs("[", ctx->out);
                            for (int i = 0; i < node->as.call.arg_count; i++) {
                                if (i > 0) fputs("][", ctx->out);
                                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                            }
                            fputs("]))", ctx->out);
                        } else {
                            emit_expr(node->as.call.callee, ctx);
                            fputs("[", ctx->out);
                            for (int i = 0; i < node->as.call.arg_count; i++) {
                                if (i > 0) fputs("][", ctx->out);
                                if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                            }
                            fputs("]", ctx->out);
                        }
                    } else {
                        fputs("((const int64_t*)(((TiqSlice)(", ctx->out);
                        emit_expr(node->as.call.callee, ctx);
                        fputs(")).ptr))[", ctx->out);
                        if (node->as.call.arg_count > 0 && node->as.call.args[0])
                            emit_expr(node->as.call.args[0], ctx);
                        else
                            fputs("0", ctx->out);
                        fputs("]", ctx->out);
                    }
                    break;
                }
                if (node->as.call.callee->kind == AST_IDENTIFIER) {
                    fprintf(ctx->out, "tiq_gen_%.*s(", (int)node->as.call.callee->as.identifier.name.length, node->as.call.callee->as.identifier.name.start);
                } else if (node->as.call.callee->kind == AST_CALL &&
                           node->as.call.callee->as.call.callee &&
                           node->as.call.callee->as.call.callee->kind == AST_IDENTIFIER) {
                    AstNode *inner_fn = node->as.call.callee->as.call.callee;
                    int dummy1, fn_param_count;
                    if (is_stream_gen_name(ctx, inner_fn->as.identifier.name.start,
                                           (int)inner_fn->as.identifier.name.length,
                                           &dummy1, &fn_param_count)) {
                        fprintf(ctx->out, "tiq_gen_%.*s(", (int)inner_fn->as.identifier.name.length, inner_fn->as.identifier.name.start);
                        for (int ai = 0; ai < node->as.call.callee->as.call.arg_count; ai++) {
                            if (ai > 0) fputs(", ", ctx->out);
                            if (node->as.call.callee->as.call.args[ai])
                                emit_expr(node->as.call.callee->as.call.args[ai], ctx);
                        }
                        if (node->as.call.arg_count > 0) fputs(", ", ctx->out);
                    } else {
                        emit_expr(node->as.call.callee, ctx);
                        fputs("(", ctx->out);
                    }
                } else {
                    emit_expr(node->as.call.callee, ctx);
                    fputs("(", ctx->out);
                }
            } else {
                emit_expr(node->as.call.callee, ctx);
                fputs("(", ctx->out);
            }
            for (int i = 0; i < node->as.call.arg_count; i++) {
                if (i > 0) fputs(", ", ctx->out);
                SemanticType *arg_t = node->as.call.args[i] ? node->as.call.args[i]->semantic_type : NULL;
                if (arg_t && arg_t->kind == TYPE_ARRAY && !node->as.call.is_bracket_call) {
                    fputs("(TiqSlice){ .ptr = ", ctx->out);
                    emit_expr(node->as.call.args[i], ctx);
                    fprintf(ctx->out, ", .len = %d }", arg_t->array_length);
                } else {
                    if (node->as.call.args[i]) emit_expr(node->as.call.args[i], ctx);
                }
            }
            fputs(")", ctx->out);
            break;
        case AST_ARRAY: {
            fputs("{", ctx->out);
            for (int i = 0; i < node->as.array.element_count; i++) {
                if (i > 0) fputs(", ", ctx->out);
                emit_expr(node->as.array.elements[i], ctx);
            }
            fputs("}", ctx->out);
            break;
        }
        case AST_ARRAY_FILL: {
            // For array fill [value; length], we need to emit all elements explicitly
            // because C initializers only zero-initialize the rest.
            // Check if we can determine the length at compile time.
            int len = 0;
            SemanticType *t = node->semantic_type;
            if (t && t->kind == TYPE_ARRAY) {
                len = t->array_length;
            }
            if (len > 0) {
                // We know the length: emit explicit element list
                fputs("{ ", ctx->out);
                emit_expr(node->as.array_fill.value, ctx);
                for (int i = 1; i < len; i++) {
                    fputs(", ", ctx->out);
                    emit_expr(node->as.array_fill.value, ctx);
                }
                fputs(" }", ctx->out);
            } else {
                // Unknown length: emit just the first element (defensive)
                // This case shouldn't normally occur with proper type inference
                fputs("{ ", ctx->out);
                emit_expr(node->as.array_fill.value, ctx);
                fputs(" }", ctx->out);
            }
            break;
        }
        case AST_FIELD_ACCESS:
            emit_expr(node->as.field_access.target, ctx);
            fputs(".", ctx->out);
            fprintf(ctx->out, "%.*s", (int)node->as.field_access.field.length, node->as.field_access.field.start);
            break;
        case AST_RECORD_LIT: {
            // M12.6: Emit C struct initializer
            SemanticType *st = node->semantic_type;
            if (st && st->kind == TYPE_STRUCT && st->struct_name) {
                fprintf(ctx->out, "(%s){ ", st->struct_name);
                for (int i = 0; i < node->as.record_lit.field_count; i++) {
                    if (i > 0) fputs(", ", ctx->out);
                    fprintf(ctx->out, ".%.*s = ", (int)node->as.record_lit.field_names[i].length,
                            node->as.record_lit.field_names[i].start);
                    emit_expr(node->as.record_lit.field_values[i], ctx);
                }
                fputs(" }", ctx->out);
            } else {
                fputs("0", ctx->out);
            }
            break;
        }
        case AST_SPAWN:
            // Unreachable after semantic rejection; fail closed if hit.
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "spawn is not supported yet");
            break;
        case AST_CHAN:
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "chan is not supported yet");
            break;
        case AST_MATCH: {
            // Emit: (cond1 ? body1 : (cond2 ? body2 : (... : 0)))
            // Wildcard arms match everything and should be the last arm
            // Structure: (cond) ? then : else  (parens close right after condition)
            bool has_wildcard = false;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                if (node->as.match_expr.arms[i].is_wildcard) {
                    emit_expr(node->as.match_expr.arms[i].body, ctx);
                    has_wildcard = true;
                } else {
                    fputs("(", ctx->out);
                    emit_expr(node->as.match_expr.expr, ctx);
                    fputs(" == ", ctx->out);
                    emit_expr(node->as.match_expr.arms[i].pattern, ctx);
                    fputs(") ? ", ctx->out);
                    emit_expr(node->as.match_expr.arms[i].body, ctx);
                    fputs(" : ", ctx->out);
                }
            }
            // Final fallback (only if no wildcard arm)
            if (!has_wildcard) {
                fputs("0", ctx->out);
            }
            break;
        }
        case AST_BLOCK:
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                       "block expression not supported outside function body");
            break;
        case AST_DEFER:
            if (node->as.defer.expr)
                emit_expr(node->as.defer.expr, ctx);
            break;
        default:
            break;
    }
}

static void emit_type_name(PrimitiveType kind, FILE *out) {
    switch (kind) {
        case TYPE_INT:      fputs("int64_t", out); break;
        case TYPE_FLOAT:    fputs("double", out); break;
        case TYPE_BOOL:     fputs("int64_t", out); break;
        case TYPE_STR:      fputs("const char *", out); break;
        case TYPE_ARRAY:    fputs("int64_t", out); break;
        case TYPE_SLICE:    fputs("TiqSlice", out); break;
        case TYPE_STR_VIEW: fputs("TiqSlice", out); break;
        case TYPE_STREAM:   fputs("int64_t", out); break;
        // Sized kinds map to stdint.h types (M12.2); no surface syntax
        // constructs them until explicit conversions land (M12.3).
        case TYPE_I8:       fputs("int8_t", out); break;
        case TYPE_I16:      fputs("int16_t", out); break;
        case TYPE_I32:      fputs("int32_t", out); break;
        case TYPE_U8:       fputs("uint8_t", out); break;
        case TYPE_U16:      fputs("uint16_t", out); break;
        case TYPE_U32:      fputs("uint32_t", out); break;
        case TYPE_U64:      fputs("uint64_t", out); break;
        case TYPE_F32:      fputs("float", out); break;
        case TYPE_UNIT:     fputs("void", out); break;
        case TYPE_NEVER:    fputs("void", out); break;
        case TYPE_OPTION:   fputs("TiqOption", out); break;
        case TYPE_RESULT:   fputs("TiqResult", out); break;
        default:           fputs("int64_t", out); break;
    }
}

// M12.6/M8: Emit full C type for a SemanticType, including struct names.
static void emit_semantic_type(SemanticType *t, FILE *out) {
    if (!t) { fputs("int64_t", out); return; }
    if (t->kind == TYPE_REF || t->kind == TYPE_REF_MUT) {
        // M9.1: &T -> const T *, &mut T -> T *.
        if (t->kind == TYPE_REF) fputs("const ", out);
        if (t->element_type) emit_type_name(t->element_type->kind, out);
        else fputs("int64_t", out);
        fputs(" *", out);
        return;
    }
    if (t->kind == TYPE_STRUCT && t->struct_name && t->struct_name[0]) {
        fputs(t->struct_name, out);
    } else {
        emit_type_name(t->kind, out);
    }
}

static void emit_stmt(AstNode *node, EmitContext *ctx, int indent) {
    if (!node) return;
    // M9.2-F: hoist the unbound owned temporaries of a simple statement
    // into hidden bindings before it; they are freed right after it. On
    // overflow, hoist nothing for this statement: leak, never dangle.
    int hoist_start = ctx->hoist_count;
    {
        bool overflow = false;
        if (node->kind == AST_BINDING)
            hoist_collect(node->as.binding.expr, true, ctx, &overflow);
        else if (node->kind == AST_ASSIGN)
            hoist_collect(node->as.assign.expr, true, ctx, &overflow);
        else if (node->kind == AST_CALL && !is_proc_exit_call(node))
            hoist_collect(node, false, ctx, &overflow);
        else if (node->kind == AST_CONDITIONAL)
            hoist_collect(node, false, ctx, &overflow);
        if (overflow) ctx->hoist_count = hoist_start;
    }
    for (int k = hoist_start; k < ctx->hoist_count; k++) {
        for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
        fprintf(ctx->out, "const char *tiq_tmp%d = ", ctx->hoist_ids[k]);
        ctx->hoist_emitting = ctx->hoisted[k];
        emit_expr(ctx->hoisted[k], ctx);
        ctx->hoist_emitting = NULL;
        fputs(";\n", ctx->out);
    }
    for (int i = 0; i < indent; i++) fputs("    ", ctx->out);
    switch (node->kind) {
        case AST_BINDING: {
            SemanticType *t = node->semantic_type;
            bool is_move = node->as.binding.expr && node->as.binding.expr->kind == AST_UNARY &&
                           node->as.binding.expr->as.unary.op == TOK_MOVE;
            if (t && t->kind == TYPE_ARRAY) {
                if (t->element_type) emit_type_name(t->element_type->kind, ctx->out);
                else fputs("int64_t", ctx->out);
                int arr_len = t->array_length > 0 ? t->array_length : 0;
                fprintf(ctx->out, " %.*s[%d]", (int)node->as.binding.name.length, node->as.binding.name.start,
                        arr_len);
                if (is_move && node->as.binding.expr->as.unary.right &&
                    node->as.binding.expr->as.unary.right->kind == AST_IDENTIFIER) {
                    fputs(";\n", ctx->out);
                    for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
                    fputs("memcpy(", ctx->out);
                    fprintf(ctx->out, "%.*s, ", (int)node->as.binding.name.length, node->as.binding.name.start);
                    AstNode *src = node->as.binding.expr->as.unary.right;
                    fprintf(ctx->out, "%.*s, sizeof(int64_t) * %d);\n",
                            (int)src->as.identifier.name.length, src->as.identifier.name.start, arr_len);
                } else {
                    fputs(" = ", ctx->out);
                    emit_expr(node->as.binding.expr, ctx);
                    fputs(";\n", ctx->out);
                }
            } else if (t && t->kind == TYPE_STRUCT && t->struct_name) {
                // M12.6: Struct binding
                fprintf(ctx->out, "%s %.*s", t->struct_name,
                        (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            } else if (t && t->kind == TYPE_OPTION) {
                // M8: Option binding
                fprintf(ctx->out, "TiqOption %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            } else if (t && t->kind == TYPE_RESULT) {
                // M8: Result binding
                fprintf(ctx->out, "TiqResult %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            } else {
                if (t) emit_type_name(t->kind, ctx->out);
                else fputs("int64_t", ctx->out);
                fprintf(ctx->out, " %.*s", (int)node->as.binding.name.length, node->as.binding.name.start);
                fputs(" = ", ctx->out);
                emit_expr(node->as.binding.expr, ctx);
                fputs(";\n", ctx->out);
            }
            break;
        }
        case AST_ASSIGN:
            {
                if (node->as.assign.is_definition) {
                    SemanticType *st = node->semantic_type;
                    if (st && st->kind == TYPE_ARRAY) {
                        if (st->element_type) emit_type_name(st->element_type->kind, ctx->out);
                        else fputs("int64_t", ctx->out);
                        int arr_len = st->array_length > 0 ? st->array_length : 0;
                        fprintf(ctx->out, " %.*s[%d] = ", (int)node->as.assign.name.length, node->as.assign.name.start, arr_len);
                        emit_expr(node->as.assign.expr, ctx);
                        fputs(";\n", ctx->out);
                    } else {
                        if (st) emit_type_name(st->kind, ctx->out); else fputs("int64_t", ctx->out);
                        fprintf(ctx->out, " %.*s = ", (int)node->as.assign.name.length, node->as.assign.name.start);
                        emit_expr(node->as.assign.expr, ctx);
                        fputs(";\n", ctx->out);
                    }
                    break;
                }
                int arr_len = 0;
                SemanticType *st = node->semantic_type;
                if (st && st->kind == TYPE_ARRAY) arr_len = st->array_length;
                // M9.2-D: reassigning a qualifying mutable owner destroys the
                // previous string after the new value is computed (§16.4).
                if (!node->as.assign.index && node->as.assign.op == TOK_LARROW &&
                    is_owned_str_builtin_call(node->as.assign.expr) &&
                    mut_reassign_owner(ctx, node->as.assign.name)) {
                    fprintf(ctx->out, "{ const char *tiq_old = %.*s; %.*s = ",
                            (int)node->as.assign.name.length, node->as.assign.name.start,
                            (int)node->as.assign.name.length, node->as.assign.name.start);
                    emit_expr(node->as.assign.expr, ctx);
                    fputs("; free((void *)tiq_old); }\n", ctx->out);
                    break;
                }
                if (node->as.assign.index && arr_len > 0) {
                    fputs("if ((uint64_t)(", ctx->out);
                    emit_expr(node->as.assign.index, ctx);
                    fprintf(ctx->out, ") >= (uint64_t)(%d)) { ", arr_len);
                    fprintf(ctx->out, "fprintf(stderr, \"tiq: index out of bounds for array of length %d\\n\"); exit(1); }\n", arr_len);
                    for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
                }
                if (!node->as.assign.index && ref_param_kind(ctx, node->as.assign.name) != 0) {
                    // M9.1: assignment through a mutable borrow dereferences.
                    fprintf(ctx->out, "(*%.*s)", (int)node->as.assign.name.length, node->as.assign.name.start);
                } else {
                    fprintf(ctx->out, "%.*s", (int)node->as.assign.name.length, node->as.assign.name.start);
                }
                if (node->as.assign.index) {
                    fputs("[", ctx->out);
                    emit_expr(node->as.assign.index, ctx);
                    fputs("]", ctx->out);
                }
            }
            fputs(" ", ctx->out);
            {
                TokenKind op = node->as.assign.op;
                if (op == TOK_LARROW) fputs("=", ctx->out);
                else if (op == TOK_PLUS_EQ) fputs("+=", ctx->out);
                else if (op == TOK_MINUS_EQ) fputs("-=", ctx->out);
                else if (op == TOK_STAR_EQ) fputs("*=", ctx->out);
                else if (op == TOK_SLASH_EQ) fputs("/=", ctx->out);
                else if (op == TOK_PERCENT_EQ) fputs("%=", ctx->out);
                else fputs("=", ctx->out);
            }
            fputs(" ", ctx->out);
            emit_expr(node->as.assign.expr, ctx);
            fputs(";\n", ctx->out);
            break;
        case AST_FUNCTION:
            break;
        case AST_STRUCT_DEF:
            // M12.6: Struct definitions are emitted at the top level, not in statements
            break;
        case AST_BREAK:
            // M9.2-B: destroy owned strings of every scope this break exits.
            emit_jump_frees(ctx, indent);
            fputs("break;\n", ctx->out);
            break;
        case AST_SKIP:
            emit_jump_frees(ctx, indent);
            fputs("continue;\n", ctx->out);
            break;
        case AST_BRACKET_LOOP: {
            AstNode *domain = node->as.bracket_loop.domain;
            bool is_range = domain && domain->kind == AST_BINARY && domain->as.binary.op == TOK_DOT_DOT;
            if (is_range) {
                const char *var = node->as.bracket_loop.has_binder ?
                    node->as.bracket_loop.binder.start : "i";
                int var_len = node->as.bracket_loop.has_binder ?
                    (int)node->as.bracket_loop.binder.length : 1;
                fprintf(ctx->out, "for (int64_t %.*s = ", var_len, var);
                emit_expr(domain->as.binary.left, ctx);
                fprintf(ctx->out, "; %.*s < ", var_len, var);
                emit_expr(domain->as.binary.right, ctx);
                fprintf(ctx->out, "; %.*s++) {\n", var_len, var);
            } else {
                fputs("while (", ctx->out);
                emit_expr(domain, ctx);
                fputs(") {\n", ctx->out);
            }
            emit_scope_push(ctx, node->as.bracket_loop.body_stmts,
                            node->as.bracket_loop.body_count, true,
                            node->as.bracket_loop.body_final, NULL, 0);
            for (int i = 0; i < node->as.bracket_loop.body_count; i++) {
                if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                    ctx->scopes[ctx->scope_depth - 1].emitted = i;
                emit_stmt(node->as.bracket_loop.body_stmts[i], ctx, indent + 1);
            }
            if (node->as.bracket_loop.body_final) {
                if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                    ctx->scopes[ctx->scope_depth - 1].emitted = node->as.bracket_loop.body_count;
                emit_stmt(node->as.bracket_loop.body_final, ctx, indent + 1);
            }
            emit_scope_pop(ctx);
            // M9.2: the loop body is a scope; free its owned strings each
            // iteration (break/skip free their exited scopes at the jump).
            {
                EmitScope loop_sc = { node->as.bracket_loop.body_stmts,
                                      node->as.bracket_loop.body_count,
                                      node->as.bracket_loop.body_count, true,
                                      node->as.bracket_loop.body_final, NULL, 0 };
                emit_scope_frees(&loop_sc, loop_sc.count, ctx, indent + 1);
            }
            for (int i = 0; i < indent; i++) fputs("    ", ctx->out);
            fputs("}\n", ctx->out);
            break;
        }
        case AST_BLOCK: {
            fputs("{\n", ctx->out);
            emit_scope_push(ctx, node->as.block.statements, node->as.block.stmt_count, false,
                            node->as.block.final_expr, node->as.block.deferred,
                            node->as.block.defer_count);
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                    ctx->scopes[ctx->scope_depth - 1].emitted = i;
                emit_stmt(node->as.block.statements[i], ctx, indent + 1);
            }
            if (node->as.block.final_expr) {
                if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                    ctx->scopes[ctx->scope_depth - 1].emitted = node->as.block.stmt_count;
                emit_stmt(node->as.block.final_expr, ctx, indent + 1);
            }
            emit_scope_pop(ctx);
            for (int i = node->as.block.defer_count - 1; i >= 0; i--)
                emit_stmt(node->as.block.deferred[i], ctx, indent + 1);
            // M9.2: owned strings die after the block's defers (§16.4).
            {
                EmitScope blk_sc = { node->as.block.statements,
                                     node->as.block.stmt_count,
                                     node->as.block.stmt_count, false,
                                     node->as.block.final_expr,
                                     node->as.block.deferred,
                                     node->as.block.defer_count };
                emit_scope_frees(&blk_sc, blk_sc.count, ctx, indent + 1);
            }
            for (int i = 0; i < indent; i++) fputs("    ", ctx->out);
            fputs("}\n", ctx->out);
            break;
        }
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINARY:
        case AST_CONDITIONAL: case AST_CALL:
        case AST_STREAM_GEN: case AST_ARRAY: case AST_ARRAY_FILL:
        case AST_FIELD_ACCESS: case AST_SPAWN: case AST_CHAN: case AST_MATCH:
        case AST_UNARY:
            // M9.2-E: a statement-level proc_exit destroys the owned strings
            // of every enclosing scope before terminating; the exit code is
            // computed first so destruction cannot invalidate it (§16.4).
            if (is_proc_exit_call(node)) {
                fputs("{\n", ctx->out);
                for (int j = 0; j < indent + 1; j++) fputs("    ", ctx->out);
                fputs("int64_t tiq_exit_code = ", ctx->out);
                emit_expr(node->as.call.args[0], ctx);
                fputs(";\n", ctx->out);
                emit_exit_frees(ctx, indent + 1);
                for (int j = 0; j < indent + 1; j++) fputs("    ", ctx->out);
                fputs("tiq_proc_exit(tiq_exit_code);\n", ctx->out);
                for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
                fputs("}\n", ctx->out);
                break;
            }
            // M9.2-F: a hoisted bare statement expression reduces to its
            // hidden binding; cast to void to keep the emitted C quiet.
            for (int k = hoist_start; k < ctx->hoist_count; k++)
                if (ctx->hoisted[k] == node) { fputs("(void)", ctx->out); break; }
            emit_expr(node, ctx);
            fputs(";\n", ctx->out);
            break;
        case AST_DEFER:
            emit_stmt(node->as.defer.expr, ctx, indent);
            break;
        default:
            fputs(";\n", ctx->out);
            break;
    }
    // M9.2-F: temporaries die at the end of their statement, newest first.
    for (int k = ctx->hoist_count - 1; k >= hoist_start; k--) {
        for (int j = 0; j < indent; j++) fputs("    ", ctx->out);
        fprintf(ctx->out, "free((void *)tiq_tmp%d);\n", ctx->hoist_ids[k]);
    }
    ctx->hoist_count = hoist_start;
}

static void emit_check_node(AstNode *node, EmitContext *ctx);
static void emit_check_node(AstNode *node, EmitContext *ctx) {
    if (!node || ctx->diag->fatal_error) return;
    switch (node->kind) {
        case AST_LITERAL: case AST_IDENTIFIER: case AST_BINDING:
        case AST_ASSIGN: case AST_FUNCTION: case AST_BREAK: case AST_SKIP:
        case AST_ARRAY:
            break;
        case AST_BINARY:
            emit_check_node(node->as.binary.left, ctx);
            emit_check_node(node->as.binary.right, ctx);
            break;
        case AST_UNARY:
            emit_check_node(node->as.unary.right, ctx);
            break;
        case AST_CONDITIONAL:
            emit_check_node(node->as.conditional.cond, ctx);
            emit_check_node(node->as.conditional.then_branch, ctx);
            emit_check_node(node->as.conditional.else_branch, ctx);
            break;
        case AST_CALL:
            emit_check_node(node->as.call.callee, ctx);
            for (int i = 0; i < node->as.call.arg_count; i++)
                emit_check_node(node->as.call.args[i], ctx);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.stmt_count; i++)
                emit_check_node(node->as.block.statements[i], ctx);
            if (node->as.block.final_expr)
                emit_check_node(node->as.block.final_expr, ctx);
            break;
        case AST_BRACKET_LOOP:
            emit_check_node(node->as.bracket_loop.domain, ctx);
            for (int i = 0; i < node->as.bracket_loop.body_count; i++)
                emit_check_node(node->as.bracket_loop.body_stmts[i], ctx);
            if (node->as.bracket_loop.body_final)
                emit_check_node(node->as.bracket_loop.body_final, ctx);
            break;
        case AST_STREAM_GEN:
            for (int i = 0; i < node->as.stream_gen.seed_count; i++)
                emit_check_node(node->as.stream_gen.seeds[i], ctx);
            if (node->as.stream_gen.gen_expr)
                emit_check_node(node->as.stream_gen.gen_expr, ctx);
            if (node->as.stream_gen.bound)
                emit_check_node(node->as.stream_gen.bound, ctx);
            break;
        default:
            break;
    }
}

static void emit_stream_gen_def(const char *name, AstNode *node, Token *params, int param_count, EmitContext *ctx) {
    int sc = node->as.stream_gen.seed_count;
    if (sc == 1) {
        fprintf(ctx->out, "int64_t tiq_gen_%s(", name);
        for (int p = 0; p < param_count; p++) {
            if (p > 0) fputs(", ", ctx->out);
            fprintf(ctx->out, "int64_t %.*s", (int)params[p].length, params[p].start);
        }
        if (param_count > 0) fputs(", ", ctx->out);
        fputs("int64_t n) {\n", ctx->out);
        fputs("    if (n < 0) return 0;\n", ctx->out);
        fputs("    int64_t x = ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[0], ctx);
        fputs(";\n", ctx->out);
        fputs("    if (n == 0) return x;\n", ctx->out);
        fputs("    for (int64_t i = 1; i <= n; i++) {\n", ctx->out);
        fputs("        x = (", ctx->out);
        emit_expr(node->as.stream_gen.gen_expr, ctx);
        fputs(");\n", ctx->out);
        fputs("    }\n", ctx->out);
        fputs("    return x;\n", ctx->out);
        fputs("}\n\n", ctx->out);
    } else if (sc >= 2) {
        fprintf(ctx->out, "int64_t tiq_gen_%s(", name);
        for (int p = 0; p < param_count; p++) {
            if (p > 0) fputs(", ", ctx->out);
            fprintf(ctx->out, "int64_t %.*s", (int)params[p].length, params[p].start);
        }
        if (param_count > 0) fputs(", ", ctx->out);
        fputs("int64_t n) {\n", ctx->out);
        fputs("    if (n < 0) return 0;\n", ctx->out);
        fputs("    if (n == 0) return ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[0], ctx);
        fputs(";\n", ctx->out);
        fputs("    if (n == 1) return ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[1], ctx);
        fputs(";\n", ctx->out);
        fputs("    int64_t a = ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[1], ctx);
        fputs(";\n    int64_t b = ", ctx->out);
        emit_expr(node->as.stream_gen.seeds[0], ctx);
        fputs(";\n", ctx->out);
        fputs("    for (int64_t i = 2; i <= n; i++) {\n", ctx->out);
        fputs("        int64_t t = (", ctx->out);
        emit_expr(node->as.stream_gen.gen_expr, ctx);
        fputs(");\n", ctx->out);
        fputs("        b = a;\n", ctx->out);
        fputs("        a = t;\n", ctx->out);
        fputs("    }\n", ctx->out);
        fputs("    return a;\n", ctx->out);
        fputs("}\n\n", ctx->out);
    }
}

void compile_to_c(const char *source_path, const char *source, FILE *out, DiagContext *diag) {
    EmitContext ectx = { out, diag, source_path, {{0, 0, 0}}, 0, NULL,
                         {{NULL, 0, 0, false, NULL, NULL, 0}}, 0,
                         {NULL}, {0}, 0, NULL, 0 };
    EmitContext *ctx = &ectx;
    Parser parser;
    parser_init(&parser, source, source_path, diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    if (diag->has_error) { parser_free(&parser); return; }

    TypePool pool;
    type_pool_init(&pool);
    semantic_check(stmts, count, source_path, diag, &pool);
    if (diag->has_error) { parser_free(&parser); type_pool_free(&pool); return; }

    for (int i = 0; i < count && !diag->fatal_error; i++)
        emit_check_node(stmts[i], ctx);
    if (diag->has_error) { parser_free(&parser); type_pool_free(&pool); return; }

    // Collect stream gen bindings
    typedef struct { const char *name; AstNode *gen; Token *params; int param_count; } StreamGenDef;
    StreamGenDef stream_gens[TIQ_MAX_STREAM_GENS];
    int stream_gen_count = 0;
    for (int i = 0; i < count && stream_gen_count < TIQ_MAX_STREAM_GENS; i++) {
        if (stmts[i] && stmts[i]->kind == AST_BINDING &&
            stmts[i]->as.binding.expr && stmts[i]->as.binding.expr->kind == AST_STREAM_GEN) {
            Token n = stmts[i]->as.binding.name;
            char *sname = malloc(n.length + 1);
            memcpy(sname, n.start, n.length); sname[n.length] = '\0';
            stream_gens[stream_gen_count].name = sname;
            stream_gens[stream_gen_count].gen = stmts[i]->as.binding.expr;
            stream_gens[stream_gen_count].params = NULL;
            stream_gens[stream_gen_count].param_count = 0;
            stream_gen_count++;
        }
        // Also handle function-level streams
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION &&
            stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN &&
            stream_gen_count < TIQ_MAX_STREAM_GENS) {
            Token n = stmts[i]->as.function.name;
            char *sname = malloc(n.length + 1);
            memcpy(sname, n.start, n.length); sname[n.length] = '\0';
            stream_gens[stream_gen_count].name = sname;
            stream_gens[stream_gen_count].gen = stmts[i]->as.function.body;
            stream_gens[stream_gen_count].params = stmts[i]->as.function.params;
            stream_gens[stream_gen_count].param_count = stmts[i]->as.function.param_count;
            stream_gen_count++;
        }
    }

    int has_function = 0;
    for (int i = 0; i < count; i++)
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION) has_function = 1;

    // Populate emit-time stream gen lookup table
    ctx->stream_gen_count = 0;
    for (int g = 0; g < stream_gen_count && g < TIQ_MAX_STREAM_GENS; g++) {
        ctx->stream_gens[g].name = stream_gens[g].name;
        ctx->stream_gens[g].params = stream_gens[g].params;
        ctx->stream_gens[g].param_count = stream_gens[g].param_count;
        ctx->stream_gen_count++;
    }

    // M9.2-I: classify fresh-result functions before anything consults the
    // ownership predicates (LANGUAGE_SPEC §16.4).
    collect_fresh_str_fns(stmts, count);

    // Emit Core Language Runtime Prelude
    fputs(TIQ_CORE_RUNTIME_PRELUDE, ctx->out);

    // Emit Auxiliary Standard Library Runtime Prelude
    // (These stubs will be rewritten natively in Tiq under Milestone M19)
    fputs(TIQ_RUNTIME_PRELUDE_AUX1, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE_AUX2, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE_AUX3, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE_AUX4, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE_AUX5, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE_AUX6, ctx->out);
    fputs(TIQ_RUNTIME_PRELUDE_AUX7, ctx->out);

    // M12.6: Emit struct definitions (before function declarations so types are visible)
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind == AST_STRUCT_DEF) {
            SemanticType *st = stmts[i]->semantic_type;
            if (st && st->kind == TYPE_STRUCT && st->struct_name) {
                fprintf(ctx->out, "typedef struct {\n");
                for (int f = 0; f < st->field_count; f++) {
                    fputs("    ", ctx->out);
                    if (st->field_types[f]) emit_type_name(st->field_types[f]->kind, ctx->out);
                    else fputs("int64_t", ctx->out);
                    fprintf(ctx->out, " %s;\n", st->field_names[f]);
                }
                fprintf(ctx->out, "} %s;\n\n", st->struct_name);
            }
        }
    }

    // Forward-declare stream gen functions
    for (int g = 0; g < stream_gen_count; g++) {
        fprintf(ctx->out, "int64_t tiq_gen_%s(", stream_gens[g].name);
        for (int p = 0; p < stream_gens[g].param_count; p++) {
            if (p > 0) fputs(", ", ctx->out);
            fprintf(ctx->out, "int64_t %.*s", (int)stream_gens[g].params[p].length, stream_gens[g].params[p].start);
        }
        if (stream_gens[g].param_count > 0) fputs(", ", ctx->out);
        fputs("int64_t n);\n", ctx->out);
    }

    if (has_function) {
        for (int i = 0; i < count; i++) {
            if (stmts[i] && stmts[i]->kind == AST_FUNCTION &&
                !(stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_STREAM_GEN)) {
                SemanticType *t = stmts[i]->semantic_type;
                emit_semantic_type(t, ctx->out);
                fprintf(ctx->out, " %.*s(", (int)stmts[i]->as.function.name.length, stmts[i]->as.function.name.start);
                for (int j = 0; j < stmts[i]->as.function.param_count; j++) {
                    if (j > 0) fputs(", ", ctx->out);
                    SemanticType *pt = (SemanticType *)(stmts[i]->as.function.param_types ? stmts[i]->as.function.param_types[j] : NULL);
                    emit_semantic_type(pt, ctx->out);
                    fputs(" ", ctx->out);
                    fprintf(ctx->out, "%.*s", (int)stmts[i]->as.function.params[j].length, stmts[i]->as.function.params[j].start);
                }
                fputs(");\n", ctx->out);
            }
        }
    }

    fputs("\nint main(int argc, char **argv) {\n"
          "    tiq_argc = argc;\n"
          "    tiq_argv = argv;\n", ctx->out);

    // M9.2-D: track the top-level program scope so mutable-owner
    // reassignments inside it (and its loops) find their declaration.
    emit_scope_push(ctx, stmts, count, false, NULL, NULL, 0);
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind != AST_FUNCTION && stmts[i]->kind != AST_STRUCT_DEF) {
            if (stmts[i]->kind == AST_BINDING && stmts[i]->as.binding.expr &&
                stmts[i]->as.binding.expr->kind == AST_STREAM_GEN) {
                // stream gen bindings are emitted via tiq_gen_* functions below
                continue;
            }
            if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                ctx->scopes[ctx->scope_depth - 1].emitted = i;
            emit_stmt(stmts[i], ctx, 1);
        }
    }
    emit_scope_pop(ctx);
    // M9.2: the top-level program scope frees its owned strings before exit.
    {
        EmitScope top_sc = { stmts, count, count, false, NULL, NULL, 0 };
        emit_scope_frees(&top_sc, top_sc.count, ctx, 1);
    }
    fputs("    return 0;\n}\n\n", ctx->out);

    // Emit stream gen definitions
    for (int g = 0; g < stream_gen_count; g++)
        emit_stream_gen_def(stream_gens[g].name, stream_gens[g].gen, stream_gens[g].params, stream_gens[g].param_count, ctx);

    // Emit function definitions
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind == AST_FUNCTION && stmts[i]->as.function.body->kind != AST_STREAM_GEN) {
            ctx->current_fn = stmts[i]; // M9.1: body emission derefs ref params
            SemanticType *t = stmts[i]->semantic_type;
            emit_semantic_type(t, ctx->out);
            fprintf(ctx->out, "\n%.*s(", (int)stmts[i]->as.function.name.length, stmts[i]->as.function.name.start);
            for (int j = 0; j < stmts[i]->as.function.param_count; j++) {
                if (j > 0) fputs(", ", ctx->out);
                SemanticType *pt = (SemanticType *)(stmts[i]->as.function.param_types ? stmts[i]->as.function.param_types[j] : NULL);
                emit_semantic_type(pt, ctx->out);
                fputs(" ", ctx->out);
                fprintf(ctx->out, "%.*s", (int)stmts[i]->as.function.params[j].length, stmts[i]->as.function.params[j].start);
            }
            if (stmts[i]->as.function.body && stmts[i]->as.function.body->kind == AST_BLOCK) {
                fputs(") {\n", ctx->out);
                AstNode *block = stmts[i]->as.function.body;
                // M9.2-C: scalar-result functions destroy their owned strings
                // before returning; other result types could alias an owner,
                // so they leak instead (LANGUAGE_SPEC §16.4). An unannotated
                // function type is TYPE_UNKNOWN; the final expression's type
                // decides, and a statement final (returns 0) is scalar.
                AstNode *fe0 = block->as.block.final_expr;
                bool ret_scalar;
                if (!fe0 || fe0->kind == AST_ASSIGN || fe0->kind == AST_BINDING) {
                    ret_scalar = true;
                } else {
                    SemanticType *rt = (t && t->kind != TYPE_UNKNOWN) ? t : fe0->semantic_type;
                    ret_scalar = is_scalar_result(rt);
                }
                // M9.2-G: a str result that is a string literal or a direct
                // owned-builtin call is fresh or static storage, so it cannot
                // alias a body owner; such functions free their owners too.
                bool ret_fresh_str = false;
                if (fe0 && !ret_scalar) {
                    SemanticType *rt = (t && t->kind != TYPE_UNKNOWN) ? t : fe0->semantic_type;
                    if (rt && rt->kind == TYPE_STR)
                        ret_fresh_str =
                            (fe0->kind == AST_LITERAL && fe0->as.literal.type == TOK_STRING) ||
                            is_owned_str_builtin_call(fe0);
                }
                EmitScope fn_sc = { block->as.block.statements,
                                    block->as.block.stmt_count,
                                    block->as.block.stmt_count, false,
                                    block->as.block.final_expr,
                                    block->as.block.deferred,
                                    block->as.block.defer_count };
                // M9.2-H: a bare-identifier str result naming a body owner
                // transfers that owner to the caller; every other owner is
                // freed after the result is computed (§16.4).
                int xfer_idx = -1;
                if (fe0 && !ret_scalar && !ret_fresh_str &&
                    fe0->kind == AST_IDENTIFIER) {
                    SemanticType *rt = (t && t->kind != TYPE_UNKNOWN) ? t : fe0->semantic_type;
                    if (rt && rt->kind == TYPE_STR)
                        scope_owner_index(&fn_sc, fe0->as.identifier.name, &xfer_idx);
                }
                bool fn_frees = (ret_scalar || ret_fresh_str) && scope_has_owned(&fn_sc);
                emit_scope_push(ctx, block->as.block.statements,
                                block->as.block.stmt_count, false,
                                block->as.block.final_expr,
                                block->as.block.deferred,
                                block->as.block.defer_count);
                for (int s = 0; s < block->as.block.stmt_count; s++) {
                    if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                        ctx->scopes[ctx->scope_depth - 1].emitted = s;
                    emit_stmt(block->as.block.statements[s], ctx, 1);
                }
                if (ctx->scope_depth <= TIQ_MAX_SCOPE_DEPTH)
                    ctx->scopes[ctx->scope_depth - 1].emitted = block->as.block.stmt_count;
                for (int d = 0; d < block->as.block.defer_count; d++) {
                    emit_stmt(block->as.block.deferred[d], ctx, 1);
                }
                if (block->as.block.final_expr) {
                    // M9.1: a trailing assignment/binding is a statement, not
                    // a value; emit it and return 0 (functions default to i64).
                    AstNode *fe = block->as.block.final_expr;
                    if (fe->kind == AST_ASSIGN || fe->kind == AST_BINDING) {
                        emit_stmt(fe, ctx, 1);
                        if (fn_frees)
                            emit_scope_frees(&fn_sc, fn_sc.count, ctx, 1);
                        fputs("    return 0;\n", ctx->out);
                    } else if (fn_frees || xfer_idx >= 0) {
                        // Compute the result before the owners die.
                        fputs("    ", ctx->out);
                        emit_semantic_type((t && t->kind != TYPE_UNKNOWN) ? t : fe->semantic_type,
                                           ctx->out);
                        fputs(" tiq_fn_ret = ", ctx->out);
                        emit_expr(fe, ctx);
                        fputs(";\n", ctx->out);
                        emit_scope_frees_except(&fn_sc, fn_sc.count, xfer_idx, ctx, 1);
                        fputs("    return tiq_fn_ret;\n", ctx->out);
                    } else {
                        fputs("    return ", ctx->out);
                        emit_expr(fe, ctx);
                        fputs(";\n", ctx->out);
                    }
                } else {
                    if (fn_frees)
                        emit_scope_frees(&fn_sc, fn_sc.count, ctx, 1);
                    fputs("    return 0;\n", ctx->out);
                }
                emit_scope_pop(ctx);
                fputs("}\n\n", ctx->out);
            } else {
                fputs(") {\n    return ", ctx->out);
                emit_expr(stmts[i]->as.function.body, ctx);
                fputs(";\n}\n\n", ctx->out);
            }
            ctx->current_fn = NULL;
        }
    }

    // Free stream gen names
    for (int g = 0; g < stream_gen_count; g++) free((void*)stream_gens[g].name);

    parser_free(&parser);
    type_pool_free(&pool);
}
