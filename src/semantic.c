#include "../include/semantic.h"
#include "../include/type.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void die_oom(void) {
    fprintf(stderr, "out of memory\n");
    exit(1);
}

void env_init(Environment *env, Environment *parent) {
    env->parent = parent;
    env->symbols = NULL;
    env->count = 0;
    env->capacity = 0;
}

void env_free(Environment *env) {
    free(env->symbols);
    env->symbols = NULL;
    env->count = 0;
    env->capacity = 0;
}

bool env_define(Environment *env, Token name, bool is_mutable, SemanticType *type) {
    for (int i = 0; i < env->count; i++) {
        if (env->symbols[i].name.length == name.length &&
            memcmp(env->symbols[i].name.start, name.start, name.length) == 0) {
            return false;
        }
    }
    if (env->count + 1 > env->capacity) {
        int new_cap = env->capacity < 8 ? 8 : env->capacity * 2;
        Symbol *new_sym = realloc(env->symbols, sizeof(Symbol) * new_cap);
        if (!new_sym) die_oom();
        env->symbols = new_sym;
        env->capacity = new_cap;
    }
    env->symbols[env->count].name = name;
    env->symbols[env->count].is_mutable = is_mutable;
    env->symbols[env->count].is_moved = false;
    env->symbols[env->count].is_reserved = false;
    env->symbols[env->count].type = type;
    env->count++;
    return true;
}

// M22: define a reserved prelude symbol that users cannot redefine.
static bool env_define_reserved(Environment *env, Token name, SemanticType *type) {
    if (!env_define(env, name, false, type)) return false;
    env->symbols[env->count - 1].is_reserved = true;
    return true;
}

// M22: check whether a name collides with a reserved prelude builtin.
static bool is_reserved_name(Environment *env, Token name) {
    Symbol *sym = env_lookup(env, name);
    return sym && sym->is_reserved;
}

Symbol *env_lookup(Environment *env, Token name) {
    Environment *current = env;
    while (current) {
        for (int i = 0; i < current->count; i++) {
            if (current->symbols[i].name.length == name.length &&
                memcmp(current->symbols[i].name.start, name.start, name.length) == 0) {
                return &current->symbols[i];
            }
        }
        current = current->parent;
    }
    return NULL;
}

// M12.6: Struct registry entry
typedef struct {
    char *name;
    SemanticType *type;
} StructEntry;

// M13.1-P2: Enum registry entry; variants are stored in declaration order
// (insertion order, deterministic) and their values are their indices.
typedef struct {
    char *name;
    char **variants;
    int variant_count;
} EnumEntry;

// M9.1: Function registry entry; call sites need the definition to see
// per-parameter borrow kinds (the pooled function type is arity-only).
typedef struct {
    char *name;
    AstNode *node;
} FuncEntry;

typedef struct {
    Environment *current_env;
    const char *path;
    DiagContext *diag;
    int loop_depth;
    TypePool *pool;
    bool in_range_context;  // true when inside [...] loop/slice brackets
    bool is_std;            // M15: true when current module is a std/ library
    // M12.6: struct registry
    StructEntry *structs;
    int struct_count;
    int struct_capacity;
    // M9.1: function registry
    FuncEntry *funcs;
    int func_count;
    int func_capacity;
    // M13.1-P2: enum registry
    EnumEntry *enums;
    int enum_count;
    int enum_capacity;
} SemanticContext;

static SemanticType *ty(SemanticContext *ctx, PrimitiveType kind) {
    return type_get(ctx->pool, kind);
}

// Forward declaration for struct registry lookup (defined below)
static SemanticType *struct_lookup(SemanticContext *ctx, Token name);

// M12.4: Resolve a type annotation token to a SemanticType.
// Returns NULL if the token is not a valid type name.
static SemanticType *resolve_type_annot(SemanticContext *ctx, Token tok) {
    if (tok.kind != TOK_IDENT) return NULL;
    typedef struct { const char *name; int len; PrimitiveType kind; } TypeName;
    static const TypeName type_names[] = {
        {"i8",   2, TYPE_I8},    {"i16",  3, TYPE_I16},
        {"i32",  3, TYPE_I32},   {"i64",  3, TYPE_INT},
        {"u8",   2, TYPE_U8},    {"u16",  3, TYPE_U16},
        {"u32",  3, TYPE_U32},   {"u64",  3, TYPE_U64},
        {"f32",  3, TYPE_F32},   {"f64",  3, TYPE_FLOAT},
        {"bool", 4, TYPE_BOOL},  {"str",  3, TYPE_STR},
        {"int",  3, TYPE_INT},   {"float",5, TYPE_FLOAT},
    };
    for (int i = 0; i < (int)(sizeof type_names / sizeof type_names[0]); i++) {
        if ((int)tok.length == type_names[i].len &&
            memcmp(tok.start, type_names[i].name, tok.length) == 0) {
            return ty(ctx, type_names[i].kind);
        }
    }
    // M12.6: Check user-defined struct types
    SemanticType *st = struct_lookup(ctx, tok);
    if (st) return st;
    return NULL; // unknown type name
}

// M12.6: Struct registry helpers
static SemanticType *struct_lookup(SemanticContext *ctx, Token name) {
    for (int i = 0; i < ctx->struct_count; i++) {
        if ((int)strlen(ctx->structs[i].name) == (int)name.length &&
            memcmp(ctx->structs[i].name, name.start, name.length) == 0) {
            return ctx->structs[i].type;
        }
    }
    return NULL;
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(copy, s, len + 1);
    return copy;
}

static void struct_register(SemanticContext *ctx, const char *name, SemanticType *type) {
    if (ctx->struct_count + 1 > ctx->struct_capacity) {
        int new_cap = ctx->struct_capacity < 8 ? 8 : ctx->struct_capacity * 2;
        ctx->structs = realloc(ctx->structs, sizeof(StructEntry) * (size_t)new_cap);
        if (!ctx->structs) { fprintf(stderr, "out of memory\n"); exit(1); }
        ctx->struct_capacity = new_cap;
    }
    ctx->structs[ctx->struct_count].name = xstrdup(name);
    ctx->structs[ctx->struct_count].type = type;
    ctx->struct_count++;
}

// M13.1-P2: Enum registry helpers (LANGUAGE_SPEC §17.5).
static EnumEntry *enum_lookup(SemanticContext *ctx, Token name) {
    for (int i = 0; i < ctx->enum_count; i++) {
        if ((int)strlen(ctx->enums[i].name) == (int)name.length &&
            memcmp(ctx->enums[i].name, name.start, name.length) == 0) {
            return &ctx->enums[i];
        }
    }
    return NULL;
}

// Returns the variant's declaration index (its value), or -1 if unknown.
static int enum_variant_index(EnumEntry *e, Token variant) {
    for (int i = 0; i < e->variant_count; i++) {
        if ((int)strlen(e->variants[i]) == (int)variant.length &&
            memcmp(e->variants[i], variant.start, variant.length) == 0) {
            return i;
        }
    }
    return -1;
}

static void enum_register(SemanticContext *ctx, Token name, Token *variants, int variant_count) {
    if (ctx->enum_count + 1 > ctx->enum_capacity) {
        int new_cap = ctx->enum_capacity < 8 ? 8 : ctx->enum_capacity * 2;
        ctx->enums = realloc(ctx->enums, sizeof(EnumEntry) * (size_t)new_cap);
        if (!ctx->enums) { fprintf(stderr, "out of memory\n"); exit(1); }
        ctx->enum_capacity = new_cap;
    }
    EnumEntry *e = &ctx->enums[ctx->enum_count];
    e->name = malloc((size_t)name.length + 1);
    if (!e->name) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(e->name, name.start, name.length);
    e->name[name.length] = '\0';
    e->variant_count = variant_count;
    e->variants = NULL;
    if (variant_count > 0) {
        e->variants = malloc(sizeof(char *) * (size_t)variant_count);
        if (!e->variants) { fprintf(stderr, "out of memory\n"); exit(1); }
        for (int i = 0; i < variant_count; i++) {
            e->variants[i] = malloc((size_t)variants[i].length + 1);
            if (!e->variants[i]) { fprintf(stderr, "out of memory\n"); exit(1); }
            memcpy(e->variants[i], variants[i].start, variants[i].length);
            e->variants[i][variants[i].length] = '\0';
        }
    }
    ctx->enum_count++;
}

// M9.1: Function registry helpers.
static AstNode *func_lookup(SemanticContext *ctx, Token name) {
    // Iterate backwards so the most recent definition wins.
    for (int i = ctx->func_count - 1; i >= 0; i--) {
        if ((int)strlen(ctx->funcs[i].name) == (int)name.length &&
            memcmp(ctx->funcs[i].name, name.start, name.length) == 0) {
            return ctx->funcs[i].node;
        }
    }
    return NULL;
}

static void func_register(SemanticContext *ctx, Token name, AstNode *node) {
    if (ctx->func_count + 1 > ctx->func_capacity) {
        int new_cap = ctx->func_capacity < 8 ? 8 : ctx->func_capacity * 2;
        ctx->funcs = realloc(ctx->funcs, sizeof(FuncEntry) * (size_t)new_cap);
        if (!ctx->funcs) die_oom();
        ctx->func_capacity = new_cap;
    }
    char *s = malloc((size_t)name.length + 1);
    if (!s) die_oom();
    memcpy(s, name.start, name.length);
    s[name.length] = '\0';
    ctx->funcs[ctx->func_count].name = s;
    ctx->funcs[ctx->func_count].node = node;
    ctx->func_count++;
}

// The single kind-level compatibility rule (OPTIMIZATION_PLAN 3.1).
// Unknown unifies with anything and takes the known side; otherwise the
// kinds must match exactly. On mismatch this emits
// "<context>: expected <T>, found <U>" and returns NULL.
static SemanticType *unify(SemanticContext *ctx, int line,
                           SemanticType *expected, SemanticType *found,
                           const char *context) {
    if (!expected) return found;
    if (!found) return expected;
    if (expected->kind == TYPE_UNKNOWN) return found;
    if (found->kind == TYPE_UNKNOWN) return expected;
    if (expected->kind == found->kind) return expected;
    char want[96];
    char got[96];
    char msg[320];
    type_display(expected, want, sizeof want);
    type_display(found, got, sizeof got);
    snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
    diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
    return NULL;
}

// M16.2: FFI-safe kinds per the C ABI mapping table (LANGUAGE_SPEC §7.1).
// Everything else fails closed at the extern declaration site (E29).
static bool ffi_safe_kind(PrimitiveType k) {
    return k == TYPE_I8 || k == TYPE_I16 || k == TYPE_I32 || k == TYPE_INT ||
           k == TYPE_U8 || k == TYPE_U16 || k == TYPE_U32 || k == TYPE_U64 ||
           k == TYPE_F32 || k == TYPE_FLOAT || k == TYPE_BOOL || k == TYPE_STR ||
           k == TYPE_STRUCT;
}

static void check_node(SemanticContext *ctx, AstNode *node);
static void check_pattern(SemanticContext *ctx, Pattern *pat, SemanticType *scrutinee_type, Environment *arm_env);

// M13.1-P3: Vec builtins (LANGUAGE_SPEC §19.7). Element type T must be
// int, str, or a named struct.
static bool vec_elem_type_ok(SemanticType *t) {
    return t && (t->kind == TYPE_INT || t->kind == TYPE_STR ||
                 (t->kind == TYPE_STRUCT && t->struct_name));
}

// M13.1-P3: Element unification is stricter than unify(): struct elements
// must be the same pooled struct type (nominal), not just kind-equal.
// M13.1-P9: the comparison is structural, never pointer identity —
// type_get_func interns function return types per-arity, so an int
// returned by a user function is a distinct pooled TYPE_INT instance from
// the canonical one. Primitives compare by kind; named structs stay
// nominal (struct types are pointer-unique per name in the pool).
static bool vec_elem_same(SemanticType *expected, SemanticType *found) {
    return expected->kind == found->kind &&
           (expected->kind != TYPE_STRUCT || expected == found);
}

static void vec_unify_elem(SemanticContext *ctx, int line, SemanticType *expected,
                           SemanticType *found, const char *context) {
    if (!found || found->kind == TYPE_UNKNOWN) return;
    if (vec_elem_same(expected, found)) return;
    char want[96];
    char got[96];
    char msg[320];
    type_display(expected, want, sizeof want);
    type_display(found, got, sizeof got);
    snprintf(msg, sizeof msg, "%s: expected %s, found %s", context, want, got);
    diag_error(ctx->diag, ctx->path, line, ERR_TYPE_MISMATCH, msg);
}

// M13.1-P8: resolve container annotations (vec[T] / strbuf / map) in
// function parameter and return position (LANGUAGE_SPEC §19.10).
// Returns NULL when the token is not a container name; malformed vec
// annotations report E09 here and yield TYPE_UNKNOWN so the caller
// does not double-report.
static SemanticType *resolve_container_annot(SemanticContext *ctx, Token tok, Token elem_tok) {
    if (tok.kind != TOK_IDENT) return NULL;
    if (tok.length == 6 && memcmp(tok.start, "strbuf", 6) == 0)
        return ty(ctx, TYPE_STRBUF);
    if (tok.length == 3 && memcmp(tok.start, "map", 3) == 0)
        return ty(ctx, TYPE_MAP);
    if (tok.length == 3 && memcmp(tok.start, "vec", 3) == 0) {
        if (elem_tok.kind != TOK_IDENT) {
            diag_error(ctx->diag, ctx->path, tok.line, ERR_TYPE_MISMATCH,
                       "vec annotation requires an element type: vec[T]");
            return ty(ctx, TYPE_UNKNOWN);
        }
        SemanticType *et = resolve_type_annot(ctx, elem_tok);
        if (!et) {
            char msg[128];
            snprintf(msg, sizeof msg, "unknown type '%.*s'",
                     (int)elem_tok.length, elem_tok.start);
            diag_error(ctx->diag, ctx->path, elem_tok.line, ERR_TYPE_MISMATCH, msg);
            return ty(ctx, TYPE_UNKNOWN);
        }
        if (!vec_elem_type_ok(et)) {
            diag_error(ctx->diag, ctx->path, elem_tok.line, ERR_TYPE_MISMATCH,
                       "vec element type must be int, str, or a struct");
            return ty(ctx, TYPE_UNKNOWN);
        }
        return type_get_vec(ctx->pool, et);
    }
    return NULL;
}

// M13.1-P3: Per-builtin checker for the six vec builtins. vec_push and
// vec_set have heterogeneous signatures ((vec, T) / (vec, int, T)) that the
// generic Builtin table in AST_CALL cannot express, so each is handled
// explicitly here. The element type is established by the first vec_push on
// a binding; vec_get/vec_set/vec_pop on a vec with no established element
// type is a fail-closed E09 error (LANGUAGE_SPEC §19.7).
static void check_vec_builtin(SemanticContext *ctx, AstNode *node, Token name) {
    char nbuf[16];
    int nlen = (int)name.length < 15 ? (int)name.length : 15;
    memcpy(nbuf, name.start, (size_t)nlen);
    nbuf[nlen] = '\0';
    bool is_new = strcmp(nbuf, "vec_new") == 0;
    bool is_push = strcmp(nbuf, "vec_push") == 0;
    bool is_get = strcmp(nbuf, "vec_get") == 0;
    bool is_set = strcmp(nbuf, "vec_set") == 0;
    bool is_len = strcmp(nbuf, "vec_len") == 0;
    bool is_pop = strcmp(nbuf, "vec_pop") == 0;
    int arity = is_new ? 0 : (is_push || is_get) ? 2 : is_set ? 3 : 1;
    for (int ai = 0; ai < node->as.call.arg_count; ai++) {
        if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
    }
    if (node->as.call.arg_count != arity) {
        char msg[128];
        if (arity == 0) {
            snprintf(msg, sizeof msg, "%s expects no arguments", nbuf);
        } else {
            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                     nbuf, arity, arity == 1 ? "" : "s");
        }
        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
        node->semantic_type = is_new ? type_get_vec(ctx->pool, NULL)
                                     : ty(ctx, (is_get || is_pop) ? TYPE_UNKNOWN : TYPE_INT);
        return;
    }
    if (is_new) {
        node->semantic_type = type_get_vec(ctx->pool, NULL);
        return;
    }
    SemanticType *vt = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
    {
        char where[32];
        snprintf(where, sizeof where, "%s argument", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_VEC), vt, where);
    }
    if (is_get || is_set) {
        SemanticType *it = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        char where[32];
        snprintf(where, sizeof where, "%s index", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), it, where);
    }
    SemanticType *elem = (vt && vt->kind == TYPE_VEC) ? vt->element_type : NULL;
    if (is_push) {
        SemanticType *et = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        if (!elem) {
            if (et && et->kind != TYPE_UNKNOWN) {
                if (!vec_elem_type_ok(et)) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "vec_push element must be int, str, or a struct");
                } else if (vt && vt->kind == TYPE_VEC) {
                    // First push establishes T: repoint the binding's symbol
                    // at the interned vec<T> (pooled types are immutable).
                    SemanticType *nv = type_get_vec(ctx->pool, et);
                    if (node->as.call.args[0]->kind == AST_IDENTIFIER) {
                        Symbol *sym = env_lookup(ctx->current_env,
                                                 node->as.call.args[0]->as.identifier.name);
                        if (sym && sym->type == vt) sym->type = nv;
                    }
                    node->as.call.args[0]->semantic_type = nv;
                }
            }
        } else {
            vec_unify_elem(ctx, node->token.line, elem, et, "vec_push element");
        }
        node->semantic_type = ty(ctx, TYPE_INT);
        return;
    }
    if (is_len) {
        node->semantic_type = ty(ctx, TYPE_INT);
        return;
    }
    if (!elem) {
        // Fail closed: no element type was ever established for this vec.
        if (!vt || vt->kind == TYPE_VEC || vt->kind == TYPE_UNKNOWN) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "%s on a vec with no established element type (no vec_push yet)", nbuf);
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
        }
        node->semantic_type = ty(ctx, is_set ? TYPE_INT : TYPE_UNKNOWN);
        return;
    }
    if (is_set) {
        SemanticType *et = node->as.call.args[2] ? node->as.call.args[2]->semantic_type : NULL;
        vec_unify_elem(ctx, node->token.line, elem, et, "vec_set element");
        node->semantic_type = ty(ctx, TYPE_INT);
        return;
    }
    node->semantic_type = elem; // vec_get / vec_pop return T
}

// M13.1-P4: Per-builtin checker for the four StrBuf builtins (LANGUAGE_SPEC
// §19.8). str_buf_append's (strbuf, str) signature does not fit the generic
// Builtin table in AST_CALL, so all four are handled explicitly here, cloned
// from check_vec_builtin. TYPE_STRBUF is not parametrized, so there is no
// element-type establishment step.
static void check_strbuf_builtin(SemanticContext *ctx, AstNode *node, Token name) {
    char nbuf[16];
    int nlen = (int)name.length < 15 ? (int)name.length : 15;
    memcpy(nbuf, name.start, (size_t)nlen);
    nbuf[nlen] = '\0';
    bool is_new = strcmp(nbuf, "str_buf_new") == 0;
    bool is_append = strcmp(nbuf, "str_buf_append") == 0;
    bool is_to_str = strcmp(nbuf, "str_buf_to_str") == 0;
    int arity = is_new ? 0 : is_append ? 2 : 1;
    for (int ai = 0; ai < node->as.call.arg_count; ai++) {
        if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
    }
    if (node->as.call.arg_count != arity) {
        char msg[128];
        if (arity == 0) {
            snprintf(msg, sizeof msg, "%s expects no arguments", nbuf);
        } else {
            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                     nbuf, arity, arity == 1 ? "" : "s");
        }
        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
        node->semantic_type = ty(ctx, is_new ? TYPE_STRBUF : is_to_str ? TYPE_STR : TYPE_INT);
        return;
    }
    if (is_new) {
        node->semantic_type = ty(ctx, TYPE_STRBUF);
        return;
    }
    SemanticType *bt = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
    {
        char where[32];
        snprintf(where, sizeof where, "%s argument", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_STRBUF), bt, where);
    }
    if (is_append) {
        SemanticType *st = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_STR), st, "str_buf_append value");
    }
    node->semantic_type = ty(ctx, is_to_str ? TYPE_STR : TYPE_INT);
}

// M13.1-P5: Per-builtin checker for the seven Map builtins (LANGUAGE_SPEC
// §19.9). map_set's (map, str, int) signature does not fit the generic
// Builtin table in AST_CALL, so all seven are handled explicitly here,
// cloned from check_strbuf_builtin. TYPE_MAP is not parametrized — keys
// are always str and values always int.
static void check_map_builtin(SemanticContext *ctx, AstNode *node, Token name) {
    char nbuf[16];
    int nlen = (int)name.length < 15 ? (int)name.length : 15;
    memcpy(nbuf, name.start, (size_t)nlen);
    nbuf[nlen] = '\0';
    bool is_new = strcmp(nbuf, "map_new") == 0;
    bool is_set = strcmp(nbuf, "map_set") == 0;
    bool is_get = strcmp(nbuf, "map_get") == 0;
    bool is_has = strcmp(nbuf, "map_has") == 0;
    bool is_key_at = strcmp(nbuf, "map_key_at") == 0;
    bool is_val_at = strcmp(nbuf, "map_val_at") == 0;
    int arity = is_new ? 0 : is_set ? 3 : (is_get || is_has || is_key_at || is_val_at) ? 2 : 1;
    for (int ai = 0; ai < node->as.call.arg_count; ai++) {
        if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
    }
    PrimitiveType ret = is_new ? TYPE_MAP : is_has ? TYPE_BOOL : is_key_at ? TYPE_STR : TYPE_INT;
    if (node->as.call.arg_count != arity) {
        char msg[128];
        if (arity == 0) {
            snprintf(msg, sizeof msg, "%s expects no arguments", nbuf);
        } else {
            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                     nbuf, arity, arity == 1 ? "" : "s");
        }
        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
        node->semantic_type = ty(ctx, ret);
        return;
    }
    if (is_new) {
        node->semantic_type = ty(ctx, TYPE_MAP);
        return;
    }
    SemanticType *mt = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
    {
        char where[32];
        snprintf(where, sizeof where, "%s argument", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_MAP), mt, where);
    }
    if (is_set || is_get || is_has) {
        SemanticType *kt = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        char where[32];
        snprintf(where, sizeof where, "%s key", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_STR), kt, where);
    }
    if (is_set) {
        SemanticType *vt = node->as.call.args[2] ? node->as.call.args[2]->semantic_type : NULL;
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), vt, "map_set value");
    }
    if (is_key_at || is_val_at) {
        SemanticType *it = node->as.call.args[1] ? node->as.call.args[1]->semantic_type : NULL;
        char where[32];
        snprintf(where, sizeof where, "%s index", nbuf);
        unify(ctx, node->token.line, ty(ctx, TYPE_INT), it, where);
    }
    node->semantic_type = ty(ctx, ret);
}

static void check_node(SemanticContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_LITERAL: {
            PrimitiveType p = TYPE_UNKNOWN;
            if (node->as.literal.type == TOK_INT) {
                p = TYPE_INT;
                // Integer literals default to i64 (LANGUAGE_SPEC §11);
                // out-of-range literals are rejected at compile time.
                errno = 0;
                char *end = NULL;
                (void)strtoll(node->token.start, &end, 10);
                if (errno == ERANGE || end != node->token.start + node->token.length) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_LITERAL_RANGE,
                               "integer literal out of range for i64");
                }
            }
            else if (node->as.literal.type == TOK_FLOAT) p = TYPE_FLOAT;
            else if (node->as.literal.type == TOK_STRING) p = TYPE_STR;
            else if (node->as.literal.type == TOK_TRUE || node->as.literal.type == TOK_FALSE) p = TYPE_BOOL;
            else if (node->as.literal.type == TOK_NONE) p = TYPE_OPTION;
            node->semantic_type = ty(ctx, p);
            break;
        }
        case AST_IDENTIFIER: {
            Token id_name = node->as.identifier.name;
            Symbol *sym = env_lookup(ctx->current_env, node->as.identifier.name);
            if (!sym) {
                char msg[256];
                // M13.1-P2: a bare enum name is not a value (§17.5).
                if (enum_lookup(ctx, id_name)) {
                    snprintf(msg, sizeof(msg), "enum '%.*s' is not a value; use %.*s.<variant>",
                             (int)id_name.length, id_name.start,
                             (int)id_name.length, id_name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    break;
                }
                snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                // M15: helpful hint for gated domain builtins.
                if (!ctx->is_std) {
                    const char *p = node->as.identifier.name.start;
                    int nl = (int)node->as.identifier.name.length;
                    const char *hint = NULL;
                    if (nl >= 5 && memcmp(p, "json_", 5) == 0) hint = " \xe2\x80\x94 import \"std/json.tiq\" for JSON operations";
                    else if (nl >= 4 && memcmp(p, "net_", 4) == 0) hint = " \xe2\x80\x94 import \"std/net.tiq\" for networking";
                    else if (nl >= 5 && memcmp(p, "http_", 5) == 0) hint = " \xe2\x80\x94 import \"std/net.tiq\" for HTTP";
                    else if (nl >= 3 && memcmp(p, "ev_", 3) == 0) hint = " \xe2\x80\x94 import \"std/ev.tiq\" for event loop";
                    else if (nl >= 3 && memcmp(p, "dl_", 3) == 0) hint = " \xe2\x80\x94 import \"std/dl.tiq\" for dynamic library loading";
                    if (hint) {
                        size_t base = strlen(msg);
                        snprintf(msg + base, sizeof(msg) - base, "%s", hint);
                    }
                }
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNDEFINED_SYMBOL, msg);
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            } else if (sym->is_moved) {
                char msg[256];
                snprintf(msg, sizeof(msg), "use of moved value '%.*s'",
                         (int)node->as.identifier.name.length, node->as.identifier.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_USE_AFTER_MOVE, msg);
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            } else {
                // M9.1: reference parameters auto-deref to the referent type
                // in expression position; the emitter re-derives ref-ness
                // from the enclosing function's parameter types.
                if (sym->type && (sym->type->kind == TYPE_REF || sym->type->kind == TYPE_REF_MUT)) {
                    node->semantic_type = sym->type->element_type ?
                        sym->type->element_type : ty(ctx, TYPE_UNKNOWN);
                } else {
                    node->semantic_type = sym->type;
                }
            }
            break;
        }
        case AST_BINARY: {
            // M12.7.2: Range expressions (a..b) are only valid inside loop/slice contexts
            if (node->as.binary.op == TOK_DOT_DOT && !ctx->in_range_context) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "range expressions 'a..b' are only valid inside loop or slice contexts");
            }
            check_node(ctx, node->as.binary.left);
            check_node(ctx, node->as.binary.right);
            SemanticType *lt = node->as.binary.left ? node->as.binary.left->semantic_type : NULL;
            SemanticType *rt = node->as.binary.right ? node->as.binary.right->semantic_type : NULL;
            // M8: Fallback operator (??) - left must be Option/Result, result is inner type
            if (node->as.binary.op == TOK_QUESTION_QUESTION) {
                if (lt && (lt->kind == TYPE_OPTION || lt->kind == TYPE_RESULT)) {
                    SemanticType *inner = lt->inner_type ? lt->inner_type : ty(ctx, TYPE_UNKNOWN);
                    // Right side should match inner type (or be unknown for inference)
                    if (rt && rt->kind != TYPE_UNKNOWN && inner->kind != TYPE_UNKNOWN) {
                        unify(ctx, node->token.line, inner, rt, "fallback type mismatch");
                    }
                    node->semantic_type = inner;
                } else {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "fallback operator requires Option or Result on left side");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
                break;
            }
            if (lt && rt) {
                // Pooled types are immutable: unify() propagates inference by
                // swapping node type pointers, never by mutating types.
                SemanticType *u = unify(ctx, node->token.line, lt, rt, "type mismatch");
                if (u) {
                    if (lt->kind == TYPE_UNKNOWN) node->as.binary.left->semantic_type = u;
                    if (rt->kind == TYPE_UNKNOWN) node->as.binary.right->semantic_type = u;
                    if (node->as.binary.op == TOK_EQ_EQ || node->as.binary.op == TOK_BANG_EQ ||
                        node->as.binary.op == TOK_LT || node->as.binary.op == TOK_LTE ||
                        node->as.binary.op == TOK_GT || node->as.binary.op == TOK_GTE) {
                        node->semantic_type = ty(ctx, TYPE_BOOL);
                    } else {
                        node->semantic_type = ty(ctx, u->kind);
                    }
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            } else {
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            break;
        }
        case AST_UNARY:
            if (node->as.unary.op == TOK_MOVE) {
                check_node(ctx, node->as.unary.right);
                if (node->as.unary.right && node->as.unary.right->kind == AST_IDENTIFIER) {
                    Token name = node->as.unary.right->as.identifier.name;
                    Symbol *sym = env_lookup(ctx->current_env, name);
                    if (sym) {
                        if (!sym->is_mutable) {
                            diag_error(ctx->diag, ctx->path, node->token.line,
                                       ERR_CANNOT_MOVE_IMMUTABLE, "cannot move an immutable binding");
                        } else if (!sym->is_moved) {
                            sym->is_moved = true;
                        }
                    }
                }
                if (node->as.unary.right && node->as.unary.right->semantic_type) {
                    node->semantic_type = node->as.unary.right->semantic_type;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            } else if (node->as.unary.op == TOK_AMP) {
                // M9.1: legal borrows (&x / &mut x as arguments to reference
                // parameters) are consumed inside AST_CALL. Reaching this case
                // means the borrow appears anywhere else: fail closed, borrows
                // cannot be stored, returned, or re-borrowed (LANGUAGE_SPEC §16.3).
                check_node(ctx, node->as.unary.right);
                diag_error(ctx->diag, ctx->path, node->token.line,
                           ERR_UNSUPPORTED_STATEMENT,
                           "borrow is only valid as an argument to a reference parameter");
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            } else if (node->as.unary.op == TOK_QUESTION) {
                // M8: Propagation operator (expr?) - unwraps Option/Result.
                check_node(ctx, node->as.unary.right);
                SemanticType *rt = node->as.unary.right ?
                    node->as.unary.right->semantic_type : NULL;
                if (rt && (rt->kind == TYPE_OPTION || rt->kind == TYPE_RESULT)) {
                    node->semantic_type = rt->inner_type ? rt->inner_type : ty(ctx, TYPE_UNKNOWN);
                } else {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "propagation operator requires Option or Result operand");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            } else {
                check_node(ctx, node->as.unary.right);
                if (node->as.unary.op == TOK_BANG) {
                    // '!' is logical negation only (print is the print
                    // builtin, LANGUAGE_SPEC §12). Tiq has no truthiness,
                    // so the operand must already be bool.
                    SemanticType *rt = node->as.unary.right ?
                        node->as.unary.right->semantic_type : NULL;
                    if (rt && rt->kind != TYPE_BOOL && rt->kind != TYPE_UNKNOWN) {
                        char disp[96];
                        char msg[160];
                        type_display(rt, disp, sizeof disp);
                        snprintf(msg, sizeof msg, "operand of '!' must be bool, found %s", disp);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                    }
                    node->semantic_type = ty(ctx, TYPE_BOOL);
                } else if (node->as.unary.right && node->as.unary.right->semantic_type) {
                    node->semantic_type = node->as.unary.right->semantic_type;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_CONDITIONAL:
            check_node(ctx, node->as.conditional.cond);
            if (node->as.conditional.then_branch) check_node(ctx, node->as.conditional.then_branch);
            if (node->as.conditional.else_branch) check_node(ctx, node->as.conditional.else_branch);
            {
                SemanticType *ct = node->as.conditional.cond ?
                    node->as.conditional.cond->semantic_type : NULL;
                if (ct && ct->kind != TYPE_BOOL) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_CONDITION_TYPE,
                               "conditional condition must be bool");
                }
                SemanticType *tt = node->as.conditional.then_branch ?
                    node->as.conditional.then_branch->semantic_type : NULL;
                SemanticType *et = node->as.conditional.else_branch ?
                    node->as.conditional.else_branch->semantic_type : NULL;
                if (tt && et) {
                    SemanticType *u = unify(ctx, node->token.line, tt, et, "type mismatch");
                    if (u) {
                        if (tt->kind == TYPE_UNKNOWN) node->as.conditional.then_branch->semantic_type = u;
                        if (et->kind == TYPE_UNKNOWN) node->as.conditional.else_branch->semantic_type = u;
                        node->semantic_type = u;
                    } else {
                        node->semantic_type = tt;
                    }
                } else if (tt && !node->as.conditional.else_branch) {
                    // M25: one-arm conditional (`cond ? then`). The condition
                    // is checked for bool above; the then-branch value is
                    // discarded and the conditional's type is unit.
                    node->semantic_type = ty(ctx, TYPE_UNIT);
                } else if (tt) {
                    node->semantic_type = tt;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_CALL:
            if (node->as.call.callee && node->as.call.callee->kind == AST_IDENTIFIER) {
                Token name = node->as.call.callee->as.identifier.name;
                if (name.length == 5 && memcmp(name.start, "print", 5) == 0) {
                    // print builtin (LANGUAGE_SPEC §12): one argument of a
                    // printable type; returns the number of bytes written.
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "print expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_INT && at->kind != TYPE_FLOAT &&
                            at->kind != TYPE_BOOL && at->kind != TYPE_STR &&
                            at->kind != TYPE_STR_VIEW && at->kind != TYPE_SLICE &&
                            at->kind != TYPE_I8 && at->kind != TYPE_I16 &&
                            at->kind != TYPE_I32 && at->kind != TYPE_U8 &&
                            at->kind != TYPE_U16 && at->kind != TYPE_U32 &&
                            at->kind != TYPE_U64 && at->kind != TYPE_F32 &&
                            at->kind != TYPE_UNKNOWN) {
                            char disp[96];
                            char msg[160];
                            type_display(at, disp, sizeof disp);
                            snprintf(msg, sizeof msg, "print cannot print %s", disp);
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                        }
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                if (name.length == 3 && memcmp(name.start, "len", 3) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "len expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (!at || (at->kind != TYPE_ARRAY && at->kind != TYPE_SLICE && at->kind != TYPE_STR && at->kind != TYPE_STR_VIEW && at->kind != TYPE_UNKNOWN))
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "len expects an array argument");
                    }
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
                // M8: Option constructors
                if (name.length == 4 && memcmp(name.start, "some", 4) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "some expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_UNKNOWN) {
                            node->semantic_type = type_get_option(ctx->pool, at);
                        } else {
                            node->semantic_type = ty(ctx, TYPE_OPTION);
                        }
                    }
                    break;
                }
                if (name.length == 2 && memcmp(name.start, "ok", 2) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "ok expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_UNKNOWN) {
                            node->semantic_type = type_get_result(ctx->pool, at, ty(ctx, TYPE_INT));
                        } else {
                            node->semantic_type = ty(ctx, TYPE_RESULT);
                        }
                    }
                    break;
                }
                if (name.length == 3 && memcmp(name.start, "err", 3) == 0) {
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "err expects exactly 1 argument");
                    } else {
                        if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                        SemanticType *at = node->as.call.args[0] ?
                            node->as.call.args[0]->semantic_type : NULL;
                        if (at && at->kind != TYPE_UNKNOWN) {
                            node->semantic_type = type_get_result(ctx->pool, ty(ctx, TYPE_INT), at);
                        } else {
                            node->semantic_type = ty(ctx, TYPE_RESULT);
                        }
                    }
                    break;
                }
                // M13.1-P3: Vec builtins (LANGUAGE_SPEC §19.7). vec_push and
                // vec_set have heterogeneous signatures the table below
                // cannot express, so each is checked in check_vec_builtin.
                if ((name.length == 7 && memcmp(name.start, "vec_new", 7) == 0) ||
                    (name.length == 8 && memcmp(name.start, "vec_push", 8) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_get", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_set", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_len", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "vec_pop", 7) == 0)) {
                    check_vec_builtin(ctx, node, name);
                    break;
                }
                // M13.1-P4: StrBuf builtins (LANGUAGE_SPEC §19.8).
                // str_buf_append's (strbuf, str) signature does not fit the
                // table below, so all four go through check_strbuf_builtin.
                if ((name.length == 11 && memcmp(name.start, "str_buf_new", 11) == 0) ||
                    (name.length == 14 && memcmp(name.start, "str_buf_append", 14) == 0) ||
                    (name.length == 14 && memcmp(name.start, "str_buf_to_str", 14) == 0) ||
                    (name.length == 11 && memcmp(name.start, "str_buf_len", 11) == 0)) {
                    check_strbuf_builtin(ctx, node, name);
                    break;
                }
                // M13.1-P5: Map builtins (LANGUAGE_SPEC §19.9). map_set's
                // (map, str, int) signature does not fit the table below,
                // so all seven go through check_map_builtin.
                if ((name.length == 7 && memcmp(name.start, "map_new", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_set", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_get", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_has", 7) == 0) ||
                    (name.length == 7 && memcmp(name.start, "map_len", 7) == 0) ||
                    (name.length == 10 && memcmp(name.start, "map_key_at", 10) == 0) ||
                    (name.length == 10 && memcmp(name.start, "map_val_at", 10) == 0)) {
                    check_map_builtin(ctx, node, name);
                    break;
                }
                {
                    typedef struct {
                        const char *name; int name_len; int arity;
                        PrimitiveType expected; PrimitiveType ret;
                        // M10.3: optional distinct type for the second and
                        // later arguments; TYPE_UNKNOWN (0) means "same as
                        // expected".
                        PrimitiveType expected2;
                        // M15: gated builtins require import "std/<mod>.tiq".
                        bool gated;
                    } Builtin;
                    static const Builtin builtins[] = {
                        // Auxiliary Standard Library Primitives
                        // (Isolated stubs; under Milestone M19, these will be rewritten
                        // natively in Tiq language (`std/*.tiq`) using C FFI interop).
                        {"fs_read",        7, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, false},
                        {"fs_write",       8, 2, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, false},
                        {"fs_exists",      9, 1, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN, false},
                        {"proc_exec",      9, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, false},
                        {"proc_exit",      9, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        {"json_parse_int",14, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, true},
                        {"json_encode_str",15,1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.2: JSON decoder builtin (LANGUAGE_SPEC §19).
                        {"json_get",       8, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.3: JSON array builtins (LANGUAGE_SPEC §19).
                        {"json_arr_len",  12, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, true},
                        {"json_arr_get",  12, 2, TYPE_STR, TYPE_STR, TYPE_INT, true},
                        // M10.6: zero-copy JSON member view (LANGUAGE_SPEC §19.1).
                        // M15: stays ungated — its str_view (TiqSlice) result has
                        // no function return annotation, so it cannot be wrapped.
                        {"json_view",      9, 2, TYPE_STR, TYPE_STR_VIEW, TYPE_UNKNOWN, false},
                        // M10.7: JSON key-existence check (LANGUAGE_SPEC §19.1).
                        {"json_has",       8, 2, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN, true},
                        // M10.11: JSON object encoder (LANGUAGE_SPEC §19.1).
                        {"json_set",       8, 3, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        {"json_del",       8, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.13: String utilities (LANGUAGE_SPEC §19.5).
                        {"str_cat",        7, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, false},
                        {"int_str",        7, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, false},
                        // M13.1-P1: substring, byte equality, stderr print,
                        // directory listing (LANGUAGE_SPEC §12, §19.5, §19.6).
                        {"str_sub",        7, 3, TYPE_STR, TYPE_STR, TYPE_INT, false},
                        // M13.4-S3: byte value at index for self-hosted checker.
                        {"str_sub_code",  12, 2, TYPE_STR, TYPE_INT, TYPE_INT, false},
                        {"str_eq",         6, 2, TYPE_STR, TYPE_BOOL, TYPE_UNKNOWN, false},
                        {"eprint",         6, 1, TYPE_STR, TYPE_INT, TYPE_UNKNOWN, false},
                        {"fs_list",        7, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, false},
                        {"net_fetch",      9, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.8: TCP socket primitives (LANGUAGE_SPEC §19.3).
                        {"net_listen",    10, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_accept",    10, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_connect",   11, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_recv",       8, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, true},
                        {"net_send",       8, 2, TYPE_INT, TYPE_INT, TYPE_STR, true},
                        {"net_close",      9, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_port",       8, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        {"net_shutdown",  12, 1, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, true},
                        // M10.9: HTTP request-line parsing (LANGUAGE_SPEC §19.3).
                        {"http_method",  11, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        {"http_path",     9, 1, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        {"http_header",  11, 2, TYPE_STR, TYPE_STR, TYPE_UNKNOWN, true},
                        // M10.10: Event loop / kqueue (LANGUAGE_SPEC §19.4).
                        // M15: ev_loop stays ungated — zero-parameter functions
                        // cannot be defined in Tiq, so it cannot be wrapped.
                        {"ev_loop",       7, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        {"ev_add",        6, 2, TYPE_INT, TYPE_INT, TYPE_INT, true},
                        {"ev_wait",       7, 2, TYPE_INT, TYPE_INT, TYPE_INT, true},
                        {"ev_ready",      8, 2, TYPE_INT, TYPE_INT, TYPE_INT, true},
                        // M10.1: CLI argument builtins (LANGUAGE_SPEC §18.1)
                        {"cli_arg_count", 13, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        {"cli_arg",        7, 1, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, false},
                        // M14.3: monotonic millisecond clock (LANGUAGE_SPEC §19.6).
                        {"clock_ms",       8, 0, TYPE_INT, TYPE_INT, TYPE_UNKNOWN, false},
                        // M16.4: dynamic library loading (LANGUAGE_SPEC §19.11).
                        // dl_error stays ungated — zero-parameter functions
                        // cannot be defined in Tiq, so it cannot be wrapped
                        // (same as ev_loop).
                        {"dl_open",        7, 1, TYPE_STR, TYPE_U64, TYPE_UNKNOWN, true},
                        {"dl_sym",         6, 2, TYPE_U64, TYPE_U64, TYPE_STR, true},
                        {"dl_error",       8, 0, TYPE_INT, TYPE_STR, TYPE_UNKNOWN, false},
                        {"dl_call",        7, 7, TYPE_U64, TYPE_INT, TYPE_INT, true},
                    };
                    bool matched = false;
                    for (int bi = 0; bi < (int)(sizeof builtins / sizeof builtins[0]); bi++) {
                        if ((int)name.length != builtins[bi].name_len ||
                            memcmp(name.start, builtins[bi].name, name.length) != 0)
                            continue;
                        // M15: gate domain builtins behind std/ module import.
                        // Skip the builtin so the call resolves to the imported
                        // wrapper function instead.
                        if (builtins[bi].gated && !ctx->is_std) {
                            continue;
                        }
                        matched = true;
                        if (node->as.call.arg_count != builtins[bi].arity) {
                            char msg[128];
                            snprintf(msg, sizeof msg, "%s expects exactly %d argument%s",
                                     builtins[bi].name, builtins[bi].arity,
                                     builtins[bi].arity == 1 ? "" : "s");
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, msg);
                        } else {
                            for (int ai = 0; ai < node->as.call.arg_count; ai++) {
                                if (node->as.call.args[ai]) check_node(ctx, node->as.call.args[ai]);
                            }
                            for (int ai = 0; ai < node->as.call.arg_count; ai++) {
                                if (!node->as.call.args[ai]) continue;
                                SemanticType *at = node->as.call.args[ai]->semantic_type;
                                if (!at) continue;
                                PrimitiveType want = builtins[bi].expected;
                                if (ai >= 1 && builtins[bi].expected2 != TYPE_UNKNOWN)
                                    want = builtins[bi].expected2;
                                // str parameters also accept borrowed str_view slices.
                                if (want == TYPE_STR && at->kind == TYPE_STR_VIEW)
                                    continue;
                                char where[64];
                                snprintf(where, sizeof where, "%s argument", builtins[bi].name);
                                unify(ctx, node->token.line, ty(ctx, want), at, where);
                            }
                        }
                        node->semantic_type = ty(ctx, builtins[bi].ret);
                        break;
                    }
                    if (matched) break;
                }
                // M12.3: Explicit numeric type conversions.
                // type_name(expr) where type_name is a primitive type keyword
                // is a real checked conversion, not a function call.
                // Allowlist: numeric <-> numeric always permitted (C cast,
                // narrowing is the programmer's explicit intent).
                // bool <-> numeric and str <-> anything are rejected (E10).
                typedef struct {
                    const char *name; int len; PrimitiveType kind; const char *display;
                } ConvEntry;
                static const ConvEntry conv_table[] = {
                    {"i8",   2, TYPE_I8,    "i8"},
                    {"i16",  3, TYPE_I16,   "i16"},
                    {"i32",  3, TYPE_I32,   "i32"},
                    {"i64",  3, TYPE_INT,   "i64"},
                    {"u8",   2, TYPE_U8,    "u8"},
                    {"u16",  3, TYPE_U16,   "u16"},
                    {"u32",  3, TYPE_U32,   "u32"},
                    {"u64",  3, TYPE_U64,   "u64"},
                    {"f32",  3, TYPE_F32,   "f32"},
                    {"f64",  3, TYPE_FLOAT, "f64"},
                    {"bool", 4, TYPE_BOOL,  "bool"},
                    {"str",  3, TYPE_STR,   "str"},
                };
                static const int conv_count = (int)(sizeof conv_table / sizeof conv_table[0]);
                int ci = -1;
                for (int k = 0; k < conv_count; k++) {
                    if ((int)name.length == conv_table[k].len &&
                        memcmp(name.start, conv_table[k].name, (size_t)conv_table[k].len) == 0) {
                        ci = k; break;
                    }
                }
                if (ci >= 0) {
                    PrimitiveType tgt = conv_table[ci].kind;
                    // Arity check: exactly one argument.
                    if (node->as.call.arg_count != 1) {
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_ARITY_MISMATCH,
                                   "type conversion requires exactly 1 argument");
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        for (int i = 0; i < node->as.call.arg_count; i++) {
                            if (node->as.call.args[i]) check_node(ctx, node->as.call.args[i]);
                        }
                        break;
                    }
                    // Type-check the argument.
                    if (node->as.call.args[0]) check_node(ctx, node->as.call.args[0]);
                    SemanticType *src_t = node->as.call.args[0] ? node->as.call.args[0]->semantic_type : NULL;
                    PrimitiveType src = src_t ? src_t->kind : TYPE_UNKNOWN;
                    // Determine whether the conversion is in the allowlist.
                    // Numeric kinds: INT, FLOAT, I8, I16, I32, U8, U16, U32, U64, F32.
                    // (TYPE_I64 == TYPE_INT, TYPE_F64 == TYPE_FLOAT)
                    bool src_numeric = (src == TYPE_INT  || src == TYPE_FLOAT ||
                                        src == TYPE_I8   || src == TYPE_I16   ||
                                        src == TYPE_I32  || src == TYPE_U8    ||
                                        src == TYPE_U16  || src == TYPE_U32   ||
                                        src == TYPE_U64  || src == TYPE_F32);
                    bool tgt_numeric = (tgt == TYPE_INT  || tgt == TYPE_FLOAT ||
                                        tgt == TYPE_I8   || tgt == TYPE_I16   ||
                                        tgt == TYPE_I32  || tgt == TYPE_U8    ||
                                        tgt == TYPE_U16  || tgt == TYPE_U32   ||
                                        tgt == TYPE_U64  || tgt == TYPE_F32);
                    bool allowed = (src == TYPE_UNKNOWN)       /* unknown propagation */
                                || (src_numeric && tgt_numeric)/* numeric <-> numeric */
                                || (src == tgt);               /* identity (bool->bool, str->str) */
                    if (!allowed) {
                        // Produce a clear E10 message naming source and target types.
                        char src_name[32]; char msg[128];
                        type_display(src_t, src_name, sizeof src_name);
                        snprintf(msg, sizeof msg, "cannot convert %s to %s",
                                 src_name, conv_table[ci].display);
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_UNSUPPORTED_CONVERSION, msg);
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    }
                    node->semantic_type = ty(ctx, tgt);
                    break;
                }
            }
            check_node(ctx, node->as.call.callee);
            if (node->as.call.callee && node->as.call.callee->semantic_type) {
                SemanticType *callee_type = (SemanticType *)node->as.call.callee->semantic_type;
                if (node->as.call.is_bracket_call) {
                    for (int i = 0; i < node->as.call.arg_count; i++) {
                        if (node->as.call.args[i]) check_node(ctx, node->as.call.args[i]);
                    }
                    if (node->as.call.is_slice) {
                        if (callee_type->kind == TYPE_ARRAY || callee_type->kind == TYPE_SLICE) {
                            for (int i = 0; i < 2; i++) {
                                if (node->as.call.args[i]) {
                                    SemanticType *it = node->as.call.args[i]->semantic_type;
                                    if (it && it->kind != TYPE_INT)
                                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                                   "slice index must be int");
                                }
                            }
                            node->semantic_type = type_get_slice(ctx->pool,
                                callee_type->element_type ? callee_type->element_type : ty(ctx, TYPE_INT));
                            break;
                        } else if (callee_type->kind == TYPE_STR || callee_type->kind == TYPE_STR_VIEW) {
                            for (int i = 0; i < 2; i++) {
                                if (node->as.call.args[i]) {
                                    SemanticType *it = node->as.call.args[i]->semantic_type;
                                    if (it && it->kind != TYPE_INT)
                                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                                   "slice index must be int");
                                }
                            }
                            node->semantic_type = ty(ctx, TYPE_STR_VIEW);
                            break;
                        } else if (callee_type->kind == TYPE_STREAM) {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot range-slice a stream generator");
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                            break;
                        } else {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot slice non-array");
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                            break;
                        }
                    } else if (callee_type->kind == TYPE_ARRAY || callee_type->kind == TYPE_SLICE || callee_type->kind == TYPE_UNKNOWN) {
                        if (callee_type->kind == TYPE_UNKNOWN && node->as.call.callee->kind == AST_IDENTIFIER) {
                            Symbol *sym = env_lookup(ctx->current_env, node->as.call.callee->as.identifier.name);
                            if (sym && sym->type->kind == TYPE_UNKNOWN) {
                                sym->type = type_get_slice(ctx->pool, ty(ctx, TYPE_INT));
                                node->as.call.callee->semantic_type = sym->type;
                                callee_type = sym->type;
                            }
                        }
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "array index must be int");
                        }
                        if (callee_type->element_type)
                            node->semantic_type = callee_type->element_type;
                        else
                            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    } else if (callee_type->kind == TYPE_STR || callee_type->kind == TYPE_STR_VIEW) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "string index must be int");
                        }
                        node->semantic_type = ty(ctx, TYPE_INT);
                        break;
                    } else if (callee_type->kind == TYPE_STREAM) {
                        if (node->as.call.arg_count >= 1 && node->as.call.args[0]) {
                            SemanticType *it = node->as.call.args[0]->semantic_type;
                            if (it && it->kind != TYPE_INT)
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                           "stream index must be int");
                        }
                        node->semantic_type = ty(ctx, TYPE_INT);
                        break;
                    }
                }
                // M12.6: Skip arity check for struct-returning functions
                // (param_count is 0 for struct types; arity checked at definition)
                // M13.1-P8: likewise for vec-returning functions — their callee
                // type is the interned vec<T> (param_count 0); arity is checked
                // against the recorded definition below.
                if (!node->as.call.is_bracket_call && callee_type->kind != TYPE_STRUCT &&
                    callee_type->kind != TYPE_VEC &&
                    (callee_type->param_count > 0 ||
                     (callee_type->param_count == 0 && callee_type->kind != TYPE_UNKNOWN)) &&
                    callee_type->param_count != node->as.call.arg_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "arity mismatch");
                }
            }
            {
                // M9.1: borrow-aware argument checking. The callee definition
                // (if known) supplies per-parameter borrow kinds; borrows are
                // only legal where the parameter is a reference (§16.3).
                AstNode *fn_def = NULL;
                if (!node->as.call.is_bracket_call && node->as.call.callee &&
                    node->as.call.callee->kind == AST_IDENTIFIER) {
                    fn_def = func_lookup(ctx, node->as.call.callee->as.identifier.name);
                }
                // M13.1-P8: vec-returning functions carry vec<T> as their
                // callee type, so their arity comes from the definition.
                if (fn_def && node->as.call.callee->semantic_type &&
                    ((SemanticType *)node->as.call.callee->semantic_type)->kind == TYPE_VEC &&
                    fn_def->as.function.param_count != node->as.call.arg_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_ARITY_MISMATCH, "arity mismatch");
                }
                // Per-call aliasing bookkeeping: referent name + borrow kind.
                typedef struct { Token name; bool is_mut; } BorrowRec;
                BorrowRec *recs = NULL;
                int rec_count = 0;
                if (node->as.call.arg_count > 0) {
                    recs = malloc(sizeof(BorrowRec) * (size_t)node->as.call.arg_count);
                    if (!recs) die_oom();
                }
                for (int i = 0; i < node->as.call.arg_count; i++) {
                    AstNode *arg = node->as.call.args[i];
                    if (!arg) continue;
                    bool arg_is_borrow = arg->kind == AST_UNARY &&
                                         arg->as.unary.op == TOK_AMP;
                    int want_ref = 0; // 0=value, 1=&, 2=&mut
                    if (fn_def && i < fn_def->as.function.param_count &&
                        fn_def->as.function.param_ref_kinds)
                        want_ref = fn_def->as.function.param_ref_kinds[i];
                    if (!arg_is_borrow) {
                        check_node(ctx, arg);
                        // M16.2: extern calls check argument types against
                        // the declared C ABI signature (LANGUAGE_SPEC §7.1);
                        // containers never reach here (rejected at the decl).
                        if (fn_def && fn_def->kind == AST_EXTERN &&
                            i < fn_def->as.function.param_count &&
                            fn_def->as.function.param_types &&
                            fn_def->as.function.param_types[i]) {
                            char where[64];
                            snprintf(where, sizeof where, "argument %d", i + 1);
                            unify(ctx, node->token.line,
                                  (SemanticType *)fn_def->as.function.param_types[i],
                                  arg->semantic_type, where);
                        }
                        if (want_ref != 0) {
                            char msg[128];
                            snprintf(msg, sizeof msg, "argument %d must be borrowed with %s",
                                     i + 1, want_ref == 2 ? "&mut" : "&");
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                        // M13.1-P8: container parameters (§19.10) — the kind
                        // must match, vec elements match nominally, and an
                        // annotated vec[T] param establishes an unestablished
                        // argument vec exactly like a first vec_push would.
                        SemanticType *cpt = NULL;
                        if (fn_def && i < fn_def->as.function.param_count &&
                            fn_def->as.function.param_types)
                            cpt = fn_def->as.function.param_types[i];
                        if (cpt && (cpt->kind == TYPE_VEC || cpt->kind == TYPE_STRBUF ||
                                    cpt->kind == TYPE_MAP)) {
                            char where[64];
                            snprintf(where, sizeof where, "argument %d", i + 1);
                            SemanticType *at = arg->semantic_type;
                            if (unify(ctx, node->token.line, cpt, at, where) && at &&
                                cpt->kind == TYPE_VEC && at->kind == TYPE_VEC) {
                                if (at->element_type && cpt->element_type &&
                                    !vec_elem_same(cpt->element_type, at->element_type)) {
                                    char want[96], got[96], msg[320];
                                    type_display(cpt, want, sizeof want);
                                    type_display(at, got, sizeof got);
                                    snprintf(msg, sizeof msg, "%s: expected %s, found %s",
                                             where, want, got);
                                    diag_error(ctx->diag, ctx->path, node->token.line,
                                               ERR_TYPE_MISMATCH, msg);
                                } else if (!at->element_type && cpt->element_type) {
                                    // Establish: repoint the caller's binding.
                                    if (arg->kind == AST_IDENTIFIER) {
                                        Symbol *asym = env_lookup(ctx->current_env,
                                                                  arg->as.identifier.name);
                                        if (asym && asym->type == at) asym->type = cpt;
                                    }
                                    arg->semantic_type = cpt;
                                }
                            }
                        }
                        continue;
                    }
                    if (want_ref == 0) {
                        char msg[128];
                        snprintf(msg, sizeof msg,
                                 "argument %d cannot be a borrow: parameter is by value", i + 1);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        if (arg->as.unary.right) check_node(ctx, arg->as.unary.right);
                        arg->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        continue;
                    }
                    bool is_mut = arg->as.unary.is_mut_borrow;
                    if ((want_ref == 2) != is_mut) {
                        char msg[128];
                        snprintf(msg, sizeof msg, "argument %d must be borrowed with %s",
                                 i + 1, want_ref == 2 ? "&mut" : "&");
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                    }
                    AstNode *operand = arg->as.unary.right;
                    if (!operand || operand->kind != AST_IDENTIFIER) {
                        diag_error(ctx->diag, ctx->path, arg->token.line, ERR_BORROW,
                                   "borrow operand must be a named binding");
                        if (operand) check_node(ctx, operand);
                        arg->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        continue;
                    }
                    // Reports undefined symbol / use-after-move and yields the
                    // referent type (auto-deref never fires here except for
                    // re-borrows, which are rejected below).
                    check_node(ctx, operand);
                    Token ref_name = operand->as.identifier.name;
                    Symbol *sym = env_lookup(ctx->current_env, ref_name);
                    if (sym) {
                        if (sym->type && (sym->type->kind == TYPE_REF ||
                                          sym->type->kind == TYPE_REF_MUT)) {
                            char msg[256];
                            snprintf(msg, sizeof msg, "cannot re-borrow reference parameter '%.*s'",
                                     (int)ref_name.length, ref_name.start);
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                        if (is_mut && !sym->is_mutable) {
                            char msg[256];
                            snprintf(msg, sizeof msg, "cannot borrow immutable binding '%.*s' as mutable",
                                     (int)ref_name.length, ref_name.start);
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                        }
                    }
                    // Referent type must match the declared element type.
                    if (fn_def && i < fn_def->as.function.param_count &&
                        fn_def->as.function.param_types &&
                        fn_def->as.function.param_types[i]) {
                        SemanticType *pt = fn_def->as.function.param_types[i];
                        if ((pt->kind == TYPE_REF || pt->kind == TYPE_REF_MUT) &&
                            pt->element_type) {
                            char where[64];
                            snprintf(where, sizeof where, "argument %d", i + 1);
                            unify(ctx, node->token.line, pt->element_type,
                                  operand->semantic_type, where);
                        }
                    }
                    // Aliasing within a single call: any number of shared
                    // borrows, at most one mutable, never mixed (§16.3).
                    for (int r = 0; r < rec_count; r++) {
                        if (recs[r].name.length == ref_name.length &&
                            memcmp(recs[r].name.start, ref_name.start, ref_name.length) == 0) {
                            if (recs[r].is_mut && is_mut) {
                                char msg[256];
                                snprintf(msg, sizeof msg, "cannot borrow '%.*s' as mutable more than once in a call",
                                         (int)ref_name.length, ref_name.start);
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                                break;
                            } else if (recs[r].is_mut || is_mut) {
                                char msg[256];
                                snprintf(msg, sizeof msg, "cannot borrow '%.*s' as both mutable and shared in a call",
                                         (int)ref_name.length, ref_name.start);
                                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                                break;
                            }
                        }
                    }
                    recs[rec_count].name = ref_name;
                    recs[rec_count].is_mut = is_mut;
                    rec_count++;
                    arg->semantic_type = type_get_ref(ctx->pool,
                        operand->semantic_type ? operand->semantic_type : ty(ctx, TYPE_UNKNOWN),
                        is_mut);
                }
                free(recs);
            }
            {
                // M12.6: Use full callee type for struct returns (preserves struct_name)
                if (node->as.call.callee && node->as.call.callee->semantic_type) {
                    SemanticType *ct = node->as.call.callee->semantic_type;
                    node->semantic_type = ct;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_BLOCK: {
            Environment block_env;
            env_init(&block_env, ctx->current_env);
            ctx->current_env = &block_env;
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                check_node(ctx, node->as.block.statements[i]);
            }
            for (int i = 0; i < node->as.block.defer_count; i++) {
                check_node(ctx, node->as.block.deferred[i]);
            }
            if (node->as.block.final_expr) {
                check_node(ctx, node->as.block.final_expr);
            }
            ctx->current_env = block_env.parent;
            env_free(&block_env);
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        }
        case AST_BINDING:
            // M22: reject reserved prelude names on LHS.
            if (is_reserved_name(ctx->current_env, node->as.binding.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is a reserved builtin name",
                         (int)node->as.binding.name.length, node->as.binding.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // M22: reject enum/value namespace collision (Finding 5).
            if (enum_lookup(ctx, node->as.binding.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is already defined as an enum",
                         (int)node->as.binding.name.length, node->as.binding.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            // Issue #6: `name <- expr` (or `name = expr`) never shadows an
            // existing binding. Resolve outward via lexical lookup, then:
            //   <-  and nearest binding mutable   => reassign (rewrite to
            //                                       AST_ASSIGN so the emitter
            //                                       does not redeclare the name)
            //   <-  and nearest binding immutable => E11 (no shadow fallback)
            //   =   and a binding exists          => E11 (redefinition)
            //   no binding anywhere               => declare in current scope
            {
                Symbol *prev = env_lookup(ctx->current_env, node->as.binding.name);
                char msg[128];
                if (prev) {
                    if (!node->as.binding.is_mutable) {
                        snprintf(msg, sizeof msg, "cannot redefine binding '%.*s'",
                                 (int)node->as.binding.name.length, node->as.binding.name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_IMMUTABLE_ASSIGNMENT, msg);
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    }
                    if (!prev->is_mutable) {
                        snprintf(msg, sizeof msg, "cannot mutate immutable binding '%.*s'",
                                 (int)node->as.binding.name.length, node->as.binding.name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line,
                                   ERR_IMMUTABLE_ASSIGNMENT, msg);
                        node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                        break;
                    }
                    Token bname = node->as.binding.name;
                    AstNode *bexpr = node->as.binding.expr;
                    node->kind = AST_ASSIGN;
                    node->as.assign.name = bname;
                    node->as.assign.op = TOK_LARROW;
                    node->as.assign.index = NULL;
                    node->as.assign.expr = bexpr;
                    node->as.assign.is_definition = false;
                    check_node(ctx, node);
                    break;
                }
            }
            check_node(ctx, node->as.binding.expr);
            {
                SemanticType *type = ty(ctx, TYPE_UNKNOWN);
                if (node->as.binding.expr && node->as.binding.expr->semantic_type) {
                    type = node->as.binding.expr->semantic_type;
                }
                env_define(ctx->current_env, node->as.binding.name, node->as.binding.is_mutable, type);
                node->semantic_type = type;
            }
            break;
        case AST_ASSIGN:
            // M22: reject reserved prelude names on LHS of definitions.
            if (node->as.assign.is_definition && is_reserved_name(ctx->current_env, node->as.assign.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is a reserved builtin name",
                         (int)node->as.assign.name.length, node->as.assign.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            check_node(ctx, node->as.assign.expr);
            {
                Symbol *sym = env_lookup(ctx->current_env, node->as.assign.name);
                if (!sym) {
                    if ((node->as.assign.op == TOK_EQ || node->as.assign.op == TOK_LARROW) && !node->as.assign.index) {
                        SemanticType *type = ty(ctx, TYPE_UNKNOWN);
                        if (node->as.assign.expr && node->as.assign.expr->semantic_type) {
                            type = node->as.assign.expr->semantic_type;
                        }
                        bool is_mut = (node->as.assign.op == TOK_LARROW);
                        env_define(ctx->current_env, node->as.assign.name, is_mut, type);
                        node->as.assign.is_definition = true;
                        sym = env_lookup(ctx->current_env, node->as.assign.name);
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "undefined symbol '%.*s'",
                                 (int)node->as.assign.name.length, node->as.assign.name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNDEFINED_SYMBOL, msg);
                    }
                } else if (sym->type && sym->type->kind == TYPE_REF) {
                    // M9.1: shared borrows are read-only views.
                    char msg[256];
                    snprintf(msg, sizeof msg, "cannot assign through shared borrow '%.*s'",
                             (int)node->as.assign.name.length, node->as.assign.name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW, msg);
                } else if (sym->type && sym->type->kind == TYPE_REF_MUT) {
                    // M9.1: assignment through a mutable borrow mutates the
                    // referent in the caller; the emitter dereferences.
                    sym->is_moved = false;
                } else if (!sym->is_mutable) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_IMMUTABLE_ASSIGNMENT, "cannot assign to immutable binding");
                } else {
                    sym->is_moved = false;
                    if (node->as.assign.index) {
                        check_node(ctx, node->as.assign.index);
                        SemanticType *it = node->as.assign.index ?
                            node->as.assign.index->semantic_type : NULL;
                        if (it && it->kind != TYPE_INT)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "array index must be int");
                        if (sym->type->kind != TYPE_ARRAY && sym->type->kind != TYPE_SLICE)
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                                       "cannot index non-array");
                    }
                }
                if (sym) {
                    node->semantic_type = sym->type;
                } else {
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                }
            }
            break;
        case AST_EXTERN: {
            // M16.1/M16.2: extern "C" declaration (LANGUAGE_SPEC §7.1).
            // The ABI operand must be exactly "C" (the token includes its
            // quotes); every parameter needs an FFI-safe annotation; the
            // name must not collide with an existing declaration.
            Token name = node->as.function.name;
            if (!(node->token.kind == TOK_STRING && node->token.length == 3 &&
                  memcmp(node->token.start, "\"C\"", 3) == 0)) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                           "extern ABI must be \"C\"");
            }
            AstNode *prev = func_lookup(ctx, name);
            if (prev && prev->kind == AST_EXTERN) {
                char msg[160];
                snprintf(msg, sizeof msg, "duplicate extern declaration '%.*s'",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN, msg);
            } else if (prev || struct_lookup(ctx, name) || enum_lookup(ctx, name)) {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "extern declaration '%.*s' collides with an existing declaration",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN, msg);
            }
            for (int i = 0; i < node->as.function.param_count; i++) {
                if (node->as.function.param_ref_kinds &&
                    node->as.function.param_ref_kinds[i] != 0) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_BORROW,
                               "extern parameters cannot use borrow annotations");
                }
                Token annot = node->as.function.param_type_annots[i];
                if (annot.kind == TOK_EOF) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                               "extern parameters require type annotations");
                    continue;
                }
                if (annot.kind != TOK_IDENT) {
                    // Compound annotations ([T; N] forms) are not FFI-safe.
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                               "extern parameter type is not FFI-safe");
                    continue;
                }
                Token elem = {0};
                if (node->as.function.param_elem_annots)
                    elem = node->as.function.param_elem_annots[i];
                SemanticType *cont = resolve_container_annot(ctx, annot, elem);
                if (cont) {
                    if (cont->kind != TYPE_UNKNOWN)
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                                   "extern parameter type is not FFI-safe");
                    continue;
                }
                SemanticType *pt = resolve_type_annot(ctx, annot);
                if (!pt) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown type '%.*s'",
                             (int)annot.length, annot.start);
                    diag_error(ctx->diag, ctx->path, annot.line, ERR_TYPE_MISMATCH, msg);
                    continue;
                }
                if (!ffi_safe_kind(pt->kind)) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                               "extern parameter type is not FFI-safe");
                    continue;
                }
                if (node->as.function.param_types)
                    node->as.function.param_types[i] = pt;
            }
            // The return type is mandatory, resolved, and FFI-safe.
            SemanticType *ret = NULL;
            Token ra = node->as.function.return_type_annot;
            if (ra.kind == TOK_IDENT) {
                SemanticType *rcont = resolve_container_annot(ctx, ra,
                    node->as.function.return_elem_annot);
                if (rcont) {
                    if (rcont->kind != TYPE_UNKNOWN)
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                                   "extern return type is not FFI-safe");
                } else {
                    ret = resolve_type_annot(ctx, ra);
                    if (!ret) {
                        char msg[128];
                        snprintf(msg, sizeof msg, "unknown type '%.*s'",
                                 (int)ra.length, ra.start);
                        diag_error(ctx->diag, ctx->path, ra.line, ERR_TYPE_MISMATCH, msg);
                    } else if (!ffi_safe_kind(ret->kind)) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_EXTERN,
                                   "extern return type is not FFI-safe");
                        ret = NULL;
                    }
                }
            }
            // Register like a user function (body == NULL): calls see the
            // declared signature through the symbol and the func registry.
            PrimitiveType ret_kind = ret ? ret->kind : TYPE_UNKNOWN;
            SemanticType *func_type = (ret && ret->kind == TYPE_STRUCT) ? ret :
                type_get_func(ctx->pool, ret_kind, node->as.function.param_count);
            env_define(ctx->current_env, name, false, func_type);
            func_register(ctx, name, node);
            node->semantic_type = ret ? ret : ty(ctx, TYPE_UNKNOWN);
            break;
        }
        case AST_FUNCTION: {
            // M22: reject reserved prelude names as function names.
            if (is_reserved_name(ctx->current_env, node->as.function.name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "'%.*s' is a reserved builtin name",
                         (int)node->as.function.name.length, node->as.function.name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // M13.4-S3: resolve the return annotation before checking the
            // body so recursive calls see the correct return type.
            PrimitiveType pre_ret = TYPE_UNKNOWN;
            SemanticType *pre_ret_full = NULL;
            if (node->as.function.return_type_annot.kind == TOK_IDENT) {
                SemanticType *ra = resolve_container_annot(ctx,
                    node->as.function.return_type_annot,
                    node->as.function.return_elem_annot);
                if (!ra)
                    ra = resolve_type_annot(ctx, node->as.function.return_type_annot);
                if (ra) {
                    pre_ret = ra->kind;
                    if (ra->kind == TYPE_STRUCT || ra->kind == TYPE_VEC)
                        pre_ret_full = ra;
                }
            }
            SemanticType *func_type = pre_ret_full ? pre_ret_full :
                type_get_func(ctx->pool, pre_ret, node->as.function.param_count);
            env_define(ctx->current_env, node->as.function.name, false, func_type);
            // M9.1: record the definition so call sites can see borrow kinds.
            func_register(ctx, node->as.function.name, node);
            Environment func_env;
            env_init(&func_env, ctx->current_env);
            ctx->current_env = &func_env;
            // M12.4: Use declared parameter types from annotations if present,
            // otherwise use TYPE_UNKNOWN for inference.
            for (int i = 0; i < node->as.function.param_count; i++) {
                SemanticType *pt = ty(ctx, TYPE_UNKNOWN);
                bool is_container = false;
                // Check for type annotation
                if (node->as.function.param_type_annots &&
                    node->as.function.param_type_annots[i].kind == TOK_IDENT) {
                    // M13.1-P8: container annotations resolve before plain
                    // names (vec/strbuf/map win in annotation position).
                    Token elem_tok = {0};
                    if (node->as.function.param_elem_annots)
                        elem_tok = node->as.function.param_elem_annots[i];
                    SemanticType *cont = resolve_container_annot(ctx,
                        node->as.function.param_type_annots[i], elem_tok);
                    if (cont) {
                        pt = cont;
                        is_container = cont->kind != TYPE_UNKNOWN;
                    } else {
                        SemanticType *annot = resolve_type_annot(ctx, node->as.function.param_type_annots[i]);
                        if (annot) {
                            pt = annot;
                        } else {
                            char msg[128];
                            snprintf(msg, sizeof msg, "unknown type '%.*s'",
                                     (int)node->as.function.param_type_annots[i].length,
                                     node->as.function.param_type_annots[i].start);
                            diag_error(ctx->diag, ctx->path, node->as.function.param_type_annots[i].line,
                                       ERR_TYPE_MISMATCH, msg);
                        }
                    }
                }
                // M9.1: wrap reference parameters as &T / &mut T.
                if (node->as.function.param_ref_kinds &&
                    node->as.function.param_ref_kinds[i] != 0) {
                    if (is_container) {
                        // M13.1-P8: containers are shared handles (§19.10).
                        diag_error(ctx->diag, ctx->path,
                                   node->as.function.param_type_annots[i].line, ERR_BORROW,
                                   "container parameters are reference-semantics handles; '&' is not allowed");
                    } else {
                        pt = type_get_ref(ctx->pool, pt,
                                          node->as.function.param_ref_kinds[i] == 2);
                    }
                }
                if (node->as.function.param_types) node->as.function.param_types[i] = pt;
                env_define(ctx->current_env, node->as.function.params[i], false, pt);
            }
            check_node(ctx, node->as.function.body);
            for (int i = 0; i < node->as.function.param_count; i++) {
                Symbol *psym = env_lookup(ctx->current_env, node->as.function.params[i]);
                if (psym && node->as.function.param_types) {
                    node->as.function.param_types[i] = psym->type;
                }
            }
            ctx->current_env = func_env.parent;
            env_free(&func_env);
            PrimitiveType ret_kind = TYPE_UNKNOWN;
            if (node->as.function.body && node->as.function.body->semantic_type) {
                SemanticType *bt = (SemanticType *)node->as.function.body->semantic_type;
                ret_kind = bt->kind;
            }
            // M12.4: Check body against declared return type if present
            SemanticType *ret_type = NULL; // Full type for struct returns
            if (node->as.function.return_type_annot.kind == TOK_IDENT) {
                // M13.1-P8: container return annotations resolve first.
                SemanticType *ret_annot = resolve_container_annot(ctx,
                    node->as.function.return_type_annot,
                    node->as.function.return_elem_annot);
                if (!ret_annot)
                    ret_annot = resolve_type_annot(ctx, node->as.function.return_type_annot);
                if (ret_annot) {
                    SemanticType *u = unify(ctx, node->token.line, ret_annot,
                                            ty(ctx, ret_kind), "return type mismatch");
                    if (u) ret_kind = u->kind;
                    // M12.6: Preserve full struct type for return
                    // M13.1-P8: likewise vec<T>, so callers get the element type
                    if (ret_annot->kind == TYPE_STRUCT || ret_annot->kind == TYPE_VEC)
                        ret_type = ret_annot;
                    // M13.1-P8: nominal element check for vec expression
                    // bodies (block bodies are kind-checked only, §19.10).
                    if (ret_annot->kind == TYPE_VEC && ret_annot->element_type &&
                        node->as.function.body && node->as.function.body->semantic_type) {
                        SemanticType *bt = (SemanticType *)node->as.function.body->semantic_type;
                        if (bt->kind == TYPE_VEC && bt->element_type &&
                            !vec_elem_same(ret_annot->element_type, bt->element_type)) {
                            char want[96], got[96], msg[320];
                            type_display(ret_annot, want, sizeof want);
                            type_display(bt, got, sizeof got);
                            snprintf(msg, sizeof msg, "return type mismatch: expected %s, found %s",
                                     want, got);
                            diag_error(ctx->diag, ctx->path, node->token.line,
                                       ERR_TYPE_MISMATCH, msg);
                        }
                    }
                } else {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown return type '%.*s'",
                             (int)node->as.function.return_type_annot.length,
                             node->as.function.return_type_annot.start);
                    diag_error(ctx->diag, ctx->path, node->as.function.return_type_annot.line,
                               ERR_TYPE_MISMATCH, msg);
                }
            }
            Symbol *sym = env_lookup(ctx->current_env, node->as.function.name);
            if (sym) {
                if (pre_ret != TYPE_UNKNOWN) {
                    // M13.4-S3: annotation was pre-registered; the annotation
                    // check above already reported any mismatch.  Just ensure
                    // the symbol carries the final resolved type.
                    if (ret_type && (ret_type->kind == TYPE_STRUCT || ret_type->kind == TYPE_VEC)) {
                        sym->type = ret_type;
                    } else {
                        sym->type = type_get_func(ctx->pool, pre_ret, node->as.function.param_count);
                    }
                } else {
                    // Unify the previously recorded return type (unknown for a
                    // fresh definition) with the body type before re-pointing.
                    SemanticType *u = unify(ctx, node->token.line,
                                            ty(ctx, sym->type ? sym->type->kind : TYPE_UNKNOWN),
                                            ty(ctx, ret_kind), "type mismatch");
                    if (u && u->kind != TYPE_UNKNOWN) {
                        // M12.6: For struct returns, use the full struct type so
                        // callers can access fields. param_count is lost but
                        // arity is checked at the definition site.
                        // M13.1-P8: vec returns keep the full vec<T> the same way
                        // (arity comes from the recorded definition at call sites).
                        if (ret_type && (ret_type->kind == TYPE_STRUCT || ret_type->kind == TYPE_VEC)) {
                            sym->type = ret_type;
                        } else {
                            sym->type = type_get_func(ctx->pool, u->kind, node->as.function.param_count);
                        }
                    }
                }
            }
            // M12.6: Use full struct type if available, otherwise use kind-only type
            node->semantic_type = ret_type ? ret_type : ty(ctx, ret_kind);
            break;
        }
        case AST_BRACKET_LOOP: {
            ctx->loop_depth++;
            // M12.7.2: Set range context for domain checking
            bool was_in_range = ctx->in_range_context;
            ctx->in_range_context = true;
            check_node(ctx, node->as.bracket_loop.domain);
            ctx->in_range_context = was_in_range;
            bool is_range = node->as.bracket_loop.domain &&
                node->as.bracket_loop.domain->kind == AST_BINARY &&
                node->as.bracket_loop.domain->as.binary.op == TOK_DOT_DOT;
            if (is_range) {
                SemanticType *lt = node->as.bracket_loop.domain->as.binary.left ?
                    node->as.bracket_loop.domain->as.binary.left->semantic_type : NULL;
                SemanticType *rt = node->as.bracket_loop.domain->as.binary.right ?
                    node->as.bracket_loop.domain->as.binary.right->semantic_type : NULL;
                if ((lt && lt->kind != TYPE_INT) || (rt && rt->kind != TYPE_INT))
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "range bounds must be int");
            } else {
                SemanticType *dt = node->as.bracket_loop.domain ?
                    node->as.bracket_loop.domain->semantic_type : NULL;
                if (dt && dt->kind != TYPE_BOOL)
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_CONDITION_TYPE,
                               "loop condition must be bool");
            }
            if (node->as.bracket_loop.has_binder && !is_range) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                           "loop binder requires a range domain");
            }
            // M22: bare range loops require an explicit binder.
            if (is_range && !node->as.bracket_loop.has_binder) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                           "range loop requires an explicit binder: use [name <- domain]");
            }
            Environment loop_env;
            env_init(&loop_env, ctx->current_env);
            ctx->current_env = &loop_env;
            if (node->as.bracket_loop.has_binder) {
                // The binder replaces the implicit index; loop variables
                // are immutable inside the body.
                env_define(ctx->current_env, node->as.bracket_loop.binder, false,
                           ty(ctx, TYPE_INT));
            }
            // M22: bare range loops do NOT inject an implicit 'i'.
            // Users must write [i <- 0..10] { ... } for an index variable.
            for (int i = 0; i < node->as.bracket_loop.body_count; i++) {
                check_node(ctx, node->as.bracket_loop.body_stmts[i]);
            }
            if (node->as.bracket_loop.body_final) {
                check_node(ctx, node->as.bracket_loop.body_final);
            }
            ctx->current_env = loop_env.parent;
            env_free(&loop_env);
            ctx->loop_depth--;
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        }
        case AST_BREAK:
            if (ctx->loop_depth == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BREAK_OUTSIDE_LOOP,
                           "break outside loop");
            }
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_SKIP:
            if (ctx->loop_depth == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_BREAK_OUTSIDE_LOOP,
                           "skip outside loop");
            }
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_DEFER:
            check_node(ctx, node->as.defer.expr);
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_STREAM_GEN:
            // Stream generators support 1 or 2 seeds only (v0.1 window size)
            if (node->as.stream_gen.seed_count > 2) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "stream generators support at most 2 seeds");
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                break;
            }
            for (int i = 0; i < node->as.stream_gen.seed_count; i++) {
                check_node(ctx, node->as.stream_gen.seeds[i]);
            }
            if (node->as.stream_gen.gen_expr) {
                // M22: require explicit binders for generator expressions.
                if (node->as.stream_gen.binder_count == 0) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                               "stream generators require explicit binders: use (name) -> expr");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    break;
                }
                // Binder count must match seed count.
                if (node->as.stream_gen.binder_count != node->as.stream_gen.seed_count) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                               "binder count must match seed count");
                    node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                    break;
                }
                // Check for duplicate binder names.
                for (int b = 0; b < node->as.stream_gen.binder_count; b++) {
                    for (int b2 = b + 1; b2 < node->as.stream_gen.binder_count; b2++) {
                        if (node->as.stream_gen.binders[b].length == node->as.stream_gen.binders[b2].length &&
                            memcmp(node->as.stream_gen.binders[b].start, node->as.stream_gen.binders[b2].start,
                                   node->as.stream_gen.binders[b].length) == 0) {
                            diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                                       "duplicate stream binder name");
                        }
                    }
                    if (node->as.stream_gen.has_index_binder &&
                        node->as.stream_gen.binders[b].length == node->as.stream_gen.index_binder.length &&
                        memcmp(node->as.stream_gen.binders[b].start, node->as.stream_gen.index_binder.start,
                               node->as.stream_gen.binders[b].length) == 0) {
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_LOOP_VARIABLE,
                                   "duplicate stream binder name");
                    }
                }
                Environment gen_env;
                env_init(&gen_env, ctx->current_env);
                ctx->current_env = &gen_env;
                // Define explicit window binders.
                for (int b = 0; b < node->as.stream_gen.binder_count; b++) {
                    env_define(ctx->current_env, node->as.stream_gen.binders[b], false, ty(ctx, TYPE_INT));
                }
                // Define optional explicit index binder.
                if (node->as.stream_gen.has_index_binder) {
                    env_define(ctx->current_env, node->as.stream_gen.index_binder, false, ty(ctx, TYPE_INT));
                }
                check_node(ctx, node->as.stream_gen.gen_expr);
                ctx->current_env = gen_env.parent;
                env_free(&gen_env);
                node->semantic_type = ty(ctx, TYPE_STREAM);
            } else {
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            }
            // Stream generator bounds (while/until) are not yet implemented
            if (node->as.stream_gen.bound) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "bounded stream generators are not yet supported");
            }
            break;
        case AST_ARRAY: {
            if (node->as.array.element_count == 0) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_EMPTY_ARRAY,
                           "cannot infer element type for empty array");
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                break;
            }
            SemanticType *elem = NULL;
            for (int i = 0; i < node->as.array.element_count; i++) {
                check_node(ctx, node->as.array.elements[i]);
                SemanticType *et = node->as.array.elements[i] ?
                    node->as.array.elements[i]->semantic_type : NULL;
                SemanticType *u = unify(ctx, node->token.line, elem, et,
                                        "array elements must have the same type");
                if (u) elem = u;
            }
            node->semantic_type = type_get_array(ctx->pool,
                                                 ty(ctx, elem ? elem->kind : TYPE_UNKNOWN),
                                                 node->as.array.element_count);
            break;
        }
        case AST_ARRAY_FILL: {
            check_node(ctx, node->as.array_fill.value);
            check_node(ctx, node->as.array_fill.length);
            SemanticType *vt = node->as.array_fill.value ? (SemanticType *)node->as.array_fill.value->semantic_type : NULL;
            SemanticType *lt = node->as.array_fill.length ? (SemanticType *)node->as.array_fill.length->semantic_type : NULL;
            if (lt && lt->kind != TYPE_INT) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, "array fill length must be int");
            }
            int fill_len = 0;
            if (node->as.array_fill.length && node->as.array_fill.length->kind == AST_LITERAL &&
                node->as.array_fill.length->as.literal.type == TOK_INT) {
                // Strip underscore digit separators before parsing (lexer allows 1_000).
                const char *ls = node->as.array_fill.length->token.start;
                size_t ll = node->as.array_fill.length->token.length;
                char ltmp[64];
                size_t tl = 0;
                for (size_t i = 0; i < ll && tl < sizeof(ltmp) - 1; i++)
                    if (ls[i] != '_') ltmp[tl++] = ls[i];
                ltmp[tl] = '\0';
                errno = 0;
                char *lend = NULL;
                long long lv = strtoll(ltmp, &lend, 10);
                if (errno == ERANGE || lv < 0 || lv > 1000000) {
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_LITERAL_RANGE,
                               "array fill length out of range");
                } else {
                    fill_len = (int)lv;
                }
            }
            node->semantic_type = type_get_array(ctx->pool, ty(ctx, vt ? vt->kind : TYPE_UNKNOWN),
                                                 fill_len);
            break;
        }
        case AST_FIELD_ACCESS: {
            // M13.1-P2: Name.Variant resolves through the enum registry first;
            // an identifier target naming a declared enum wins over any
            // same-named value binding (LANGUAGE_SPEC §17.5).
            AstNode *fa_target = node->as.field_access.target;
            if (fa_target && fa_target->kind == AST_IDENTIFIER) {
                EnumEntry *ee = enum_lookup(ctx, fa_target->as.identifier.name);
                if (ee) {
                    if (enum_variant_index(ee, node->as.field_access.field) < 0) {
                        char msg[160];
                        snprintf(msg, sizeof msg, "unknown variant '%.*s' in enum '%s'",
                                 (int)node->as.field_access.field.length,
                                 node->as.field_access.field.start, ee->name);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNKNOWN_VARIANT, msg);
                    }
                    fa_target->semantic_type = ty(ctx, TYPE_INT);
                    node->semantic_type = ty(ctx, TYPE_INT);
                    break;
                }
            }
            check_node(ctx, node->as.field_access.target);
            SemanticType *tt = node->as.field_access.target ? (SemanticType *)node->as.field_access.target->semantic_type : NULL;
            SemanticType *ft = ty(ctx, TYPE_UNKNOWN);
            if (tt && tt->kind == TYPE_STRUCT) {
                bool found = false;
                for (int i = 0; i < tt->field_count; i++) {
                    if ((int)node->as.field_access.field.length == (int)strlen(tt->field_names[i]) &&
                        memcmp(node->as.field_access.field.start, tt->field_names[i], node->as.field_access.field.length) == 0) {
                        ft = tt->field_types[i] ? tt->field_types[i] : ty(ctx, TYPE_UNKNOWN);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown field '%.*s'",
                             (int)node->as.field_access.field.length,
                             node->as.field_access.field.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                }
            } else if (tt && tt->kind != TYPE_UNKNOWN) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH,
                           "field access on non-struct type");
            }
            node->semantic_type = ft;
            break;
        }
        case AST_SPAWN:
            // Fail closed: no concurrency runtime exists yet (M7), so spawn
            // must be rejected instead of compiling to a placeholder value.
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "spawn is not supported yet");
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_CHAN:
            diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT, "chan is not supported yet");
            node->semantic_type = ty(ctx, TYPE_UNKNOWN);
            break;
        case AST_IMPORT:
            // M13.1-P6: imports are resolved by the module loader before
            // checking begins; a node reaching here (single-file debug
            // paths like dump-typed-ast) is inert.
            break;
        case AST_MATCH: {
            // Pre-M13 S1: every irrefutable arm (`_` wildcard or bare
            // binding) must be the final arm. Earlier irrefutable arms
            // make every later arm unreachable and are rejected with
            // E07 before code generation.
            for (int i = 0; i < node->as.match_expr.arm_count - 1; i++) {
                Pattern *p = node->as.match_expr.arms[i].pat;
                if (p && (p->kind == PAT_WILDCARD || p->kind == PAT_BINDING)) {
                    diag_error(ctx->diag, ctx->path, p->token.line,
                               ERR_UNSUPPORTED_STATEMENT,
                               "irrefutable pattern must be the last arm");
                    break;
                }
            }
            // Wildcard requirement preserved for compatibility with the
            // M12.7.2 contract — every match must end with at least one
            // wildcard, so the irrefutable-last rule above subsumes it
            // (an early wildcard is also caught).  When the last arm is
            // a bare binding (no wildcard follows) we still require an
            // explicit `_` somewhere — keep the legacy message.
            bool has_wildcard = false;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                if (node->as.match_expr.arms[i].is_wildcard) {
                    has_wildcard = true;
                    break;
                }
            }
            if (!has_wildcard) {
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_UNSUPPORTED_STATEMENT,
                           "match must have a wildcard arm ('_ => ...')");
            }
            check_node(ctx, node->as.match_expr.expr);
            SemanticType *scrut_type = node->as.match_expr.expr ?
                node->as.match_expr.expr->semantic_type : NULL;
            SemanticType *result = NULL;
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                // Each arm gets its own scope for pattern bindings
                Environment arm_env;
                env_init(&arm_env, ctx->current_env);
                ctx->current_env = &arm_env;
                check_pattern(ctx, node->as.match_expr.arms[i].pat, scrut_type, &arm_env);
                check_node(ctx, node->as.match_expr.arms[i].body);
                ctx->current_env = arm_env.parent;
                env_free(&arm_env);
                AstNode *body = node->as.match_expr.arms[i].body;
                if (body && body->semantic_type) {
                    SemanticType *u = unify(ctx, body->token.line, result,
                                            body->semantic_type,
                                            "match arms must have the same type");
                    if (u) result = u;
                }
            }
            node->semantic_type = ty(ctx, result ? result->kind : TYPE_UNKNOWN);
            break;
        }
        case AST_STRUCT_DEF: {
            // M12.6: Register struct definition
            Token name = node->as.struct_def.name;
            // Check for duplicate struct name
            if (struct_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "duplicate struct definition '%.*s'",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // M13.1-P2: struct and enum names share a namespace (§17.5).
            if (enum_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "struct '%.*s' conflicts with enum '%.*s'",
                         (int)name.length, name.start, (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            // Resolve field types
            SemanticType **field_types = NULL;
            if (node->as.struct_def.field_count > 0) {
                field_types = malloc(sizeof(SemanticType *) * (size_t)node->as.struct_def.field_count);
                if (!field_types) { fprintf(stderr, "out of memory\n"); exit(1); }
                for (int i = 0; i < node->as.struct_def.field_count; i++) {
                    SemanticType *ft = resolve_type_annot(ctx, node->as.struct_def.field_types[i]);
                    if (!ft) {
                        char msg[128];
                        snprintf(msg, sizeof msg, "unknown field type '%.*s'",
                                 (int)node->as.struct_def.field_types[i].length,
                                 node->as.struct_def.field_types[i].start);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                        ft = ty(ctx, TYPE_UNKNOWN);
                    }
                    field_types[i] = ft;
                }
            }
            // Create and register the struct type
            SemanticType *st = type_get_struct(ctx->pool, name,
                                               node->as.struct_def.field_names,
                                               field_types,
                                               node->as.struct_def.field_count);
            char name_buf[64];
            snprintf(name_buf, sizeof name_buf, "%.*s", (int)name.length, name.start);
            struct_register(ctx, name_buf, st);
            free(field_types);
            node->semantic_type = st;
            break;
        }
        case AST_ENUM_DEF: {
            // M13.1-P2: register the enum; variants keep declaration order so
            // their values (indices) and the emitted constants are
            // deterministic (LANGUAGE_SPEC §17.5).
            Token name = node->as.enum_def.name;
            if (enum_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "duplicate enum definition '%.*s'",
                         (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            if (struct_lookup(ctx, name)) {
                char msg[128];
                snprintf(msg, sizeof msg, "enum '%.*s' conflicts with struct '%.*s'",
                         (int)name.length, name.start, (int)name.length, name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
            }
            // M22: check if enum name collides with an existing value binding (Finding 5).
            {
                Symbol *vsym = env_lookup(ctx->current_env, name);
                if (vsym && !vsym->is_reserved) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "'%.*s' is already defined as a value",
                             (int)name.length, name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_ENUM, msg);
                }
            }
            for (int i = 0; i < node->as.enum_def.variant_count; i++) {
                for (int j = 0; j < i; j++) {
                    Token a = node->as.enum_def.variants[i];
                    Token b = node->as.enum_def.variants[j];
                    if (a.length == b.length && memcmp(a.start, b.start, a.length) == 0) {
                        char msg[160];
                        snprintf(msg, sizeof msg, "duplicate variant '%.*s' in enum '%.*s'",
                                 (int)a.length, a.start, (int)name.length, name.start);
                        diag_error(ctx->diag, ctx->path, node->token.line, ERR_DUPLICATE_VARIANT, msg);
                        break;
                    }
                }
                // M22: check if variant name collides with a value binding
                // already defined in the current scope (Finding 5).
                Token v = node->as.enum_def.variants[i];
                Symbol *vsym = env_lookup(ctx->current_env, v);
                if (vsym && !vsym->is_reserved) {
                    char msg[160];
                    snprintf(msg, sizeof msg,
                             "enum variant '%.*s' shadows existing binding",
                             (int)v.length, v.start);
                    diag_error(ctx->diag, ctx->path, node->token.line,
                               ERR_DUPLICATE_VARIANT, msg);
                }
            }
            enum_register(ctx, name, node->as.enum_def.variants, node->as.enum_def.variant_count);
            break;
        }
        case AST_RECORD_LIT: {
            // M12.6: Check record literal against struct definition
            Token struct_name = node->as.record_lit.struct_name;
            SemanticType *st = struct_lookup(ctx, struct_name);
            if (!st) {
                char msg[128];
                snprintf(msg, sizeof msg, "unknown struct '%.*s'",
                         (int)struct_name.length, struct_name.start);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                node->semantic_type = ty(ctx, TYPE_UNKNOWN);
                break;
            }
            // Check field count matches
            if (node->as.record_lit.field_count != st->field_count) {
                char msg[128];
                snprintf(msg, sizeof msg, "record literal has %d fields, struct has %d",
                         node->as.record_lit.field_count, st->field_count);
                diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
            }
            // Check each field
            for (int i = 0; i < node->as.record_lit.field_count; i++) {
                Token field_name = node->as.record_lit.field_names[i];
                // Find field in struct
                int field_idx = -1;
                for (int j = 0; j < st->field_count; j++) {
                    if ((int)field_name.length == (int)strlen(st->field_names[j]) &&
                        memcmp(field_name.start, st->field_names[j], field_name.length) == 0) {
                        field_idx = j;
                        break;
                    }
                }
                if (field_idx < 0) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "unknown field '%.*s'",
                             (int)field_name.length, field_name.start);
                    diag_error(ctx->diag, ctx->path, node->token.line, ERR_TYPE_MISMATCH, msg);
                    continue;
                }
                // Check field value type
                check_node(ctx, node->as.record_lit.field_values[i]);
                SemanticType *vt = node->as.record_lit.field_values[i] ?
                    (SemanticType *)node->as.record_lit.field_values[i]->semantic_type : NULL;
                SemanticType *expected = st->field_types[field_idx];
                if (vt && expected && vt->kind != TYPE_UNKNOWN && expected->kind != TYPE_UNKNOWN) {
                    unify(ctx, node->token.line, expected, vt, "field type mismatch");
                }
            }
            node->semantic_type = st;
            break;
        }
    }
}

// Pre-M13 S1: enum variant patterns require an integer scrutinee
// (enums emit i64 constants; matching them against a non-integer
// scrutinee can never succeed and is rejected before code generation).
static bool is_integer_scrutinee_kind(SemanticType *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_INT:
        case TYPE_I8: case TYPE_I16: case TYPE_I32:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
            return true;
        default:
            return false;
    }
}

// Validate a match-arm pattern against the scrutinee type.  Bindings are
// defined in `arm_env` (the arm-local scope).  Recursive for constructor
// sub-patterns.
static void check_pattern(SemanticContext *ctx, Pattern *pat, SemanticType *scrutinee_type, Environment *arm_env) {
    if (!pat) return;
    switch (pat->kind) {
        case PAT_WILDCARD:
            // Always matches, no bindings.
            break;
        case PAT_LITERAL: {
            // Type-check the literal expression node, then unify with scrutinee.
            check_node(ctx, pat->as.literal.expr);
            SemanticType *lit_type = pat->as.literal.expr ?
                pat->as.literal.expr->semantic_type : NULL;
            if (lit_type && scrutinee_type &&
                lit_type->kind != TYPE_UNKNOWN && scrutinee_type->kind != TYPE_UNKNOWN) {
                unify(ctx, pat->token.line, scrutinee_type, lit_type,
                      "pattern type mismatch");
            }
            pat->semantic_type = scrutinee_type;
            break;
        }
        case PAT_BINDING: {
            // Define an immutable binding in the arm scope.
            // env_define returns false if the name already exists in this scope
            // (duplicate binding within the same pattern).
            if (!env_define(arm_env, pat->as.binding.name, false, scrutinee_type)) {
                char msg[160];
                snprintf(msg, sizeof msg, "duplicate binding '%.*s' in pattern",
                         (int)pat->as.binding.name.length, pat->as.binding.name.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_IMMUTABLE_ASSIGNMENT, msg);
            }
            pat->semantic_type = scrutinee_type;
            break;
        }
        case PAT_CONSTRUCTOR: {
            Token cname = pat->as.constructor.name;
            bool is_some = cname.length == 4 && memcmp(cname.start, "some", 4) == 0;
            bool is_ok   = cname.length == 2 && memcmp(cname.start, "ok", 2) == 0;
            bool is_err  = cname.length == 3 && memcmp(cname.start, "err", 3) == 0;

            if (!is_some && !is_ok && !is_err) {
                char msg[160];
                snprintf(msg, sizeof msg, "unknown constructor '%.*s'",
                         (int)cname.length, cname.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_UNDEFINED_SYMBOL, msg);
                break;
            }

            if (is_some) {
                if (!scrutinee_type || scrutinee_type->kind != TYPE_OPTION) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_TYPE_MISMATCH,
                               "'some' pattern requires Option scrutinee");
                    break;
                }
                if (pat->as.constructor.arg_count != 1) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_ARITY_MISMATCH,
                               "'some' pattern requires exactly 1 argument");
                    break;
                }
                SemanticType *inner = scrutinee_type->inner_type;
                if (!inner) inner = ty(ctx, TYPE_UNKNOWN);
                check_pattern(ctx, pat->as.constructor.args[0], inner, arm_env);
            } else if (is_ok) {
                if (!scrutinee_type || scrutinee_type->kind != TYPE_RESULT) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_TYPE_MISMATCH,
                               "'ok' pattern requires Result scrutinee");
                    break;
                }
                if (pat->as.constructor.arg_count != 1) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_ARITY_MISMATCH,
                               "'ok' pattern requires exactly 1 argument");
                    break;
                }
                SemanticType *val_type = scrutinee_type->inner_type;
                if (!val_type) val_type = ty(ctx, TYPE_UNKNOWN);
                check_pattern(ctx, pat->as.constructor.args[0], val_type, arm_env);
            } else { // is_err
                if (!scrutinee_type || scrutinee_type->kind != TYPE_RESULT) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_TYPE_MISMATCH,
                               "'err' pattern requires Result scrutinee");
                    break;
                }
                if (pat->as.constructor.arg_count != 1) {
                    diag_error(ctx->diag, ctx->path, pat->token.line,
                               ERR_ARITY_MISMATCH,
                               "'err' pattern requires exactly 1 argument");
                    break;
                }
                SemanticType *err_type = scrutinee_type->error_type;
                if (!err_type) err_type = ty(ctx, TYPE_UNKNOWN);
                check_pattern(ctx, pat->as.constructor.args[0], err_type, arm_env);
            }
            break;
        }
        case PAT_ENUM_VARIANT: {
            EnumEntry *ee = enum_lookup(ctx, pat->as.enum_variant.type_name);
            if (!ee) {
                char msg[160];
                snprintf(msg, sizeof msg, "unknown enum '%.*s'",
                         (int)pat->as.enum_variant.type_name.length,
                         pat->as.enum_variant.type_name.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_UNDEFINED_SYMBOL, msg);
                break;
            }
            if (enum_variant_index(ee, pat->as.enum_variant.variant_name) < 0) {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "unknown variant '%.*s' of enum '%.*s'",
                         (int)pat->as.enum_variant.variant_name.length,
                         pat->as.enum_variant.variant_name.start,
                         (int)pat->as.enum_variant.type_name.length,
                         pat->as.enum_variant.type_name.start);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_UNKNOWN_VARIANT, msg);
            }
            // Pre-M13 S1: enum variants are i64 constants, so the
            // scrutinee must be an integer kind — matching against str
            // (or any non-integer type) is unreachable and rejected.
            if (scrutinee_type && scrutinee_type->kind != TYPE_UNKNOWN &&
                !is_integer_scrutinee_kind(scrutinee_type)) {
                char tbuf[64];
                type_display(scrutinee_type, tbuf, sizeof tbuf);
                char msg[200];
                snprintf(msg, sizeof msg,
                         "enum variant pattern requires integer scrutinee, found %s",
                         tbuf);
                diag_error(ctx->diag, ctx->path, pat->token.line,
                           ERR_TYPE_MISMATCH, msg);
            }
            break;
        }
    }
}

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag, TypePool *pool) {
    SemanticModule mod = { stmts, count, path };
    semantic_check_modules(&mod, 1, diag, pool);
}

// M13.1-P6: check a multi-file program as one flat global namespace: the
// modules arrive in dependency post-order and share a single environment
// and struct/enum/function registry set, so cross-module duplicates hit
// the same diagnostics as duplicates within one file (§17.6). Only
// ctx.path switches per module so diagnostics name the right file.
void semantic_check_modules(SemanticModule *mods, int mod_count, DiagContext *diag, TypePool *pool) {
    SemanticContext ctx;
    ctx.path = mod_count > 0 ? mods[0].path : "<none>";
    ctx.diag = diag;
    ctx.loop_depth = 0;
    ctx.pool = pool;
    ctx.in_range_context = false;
    ctx.is_std = false;
    ctx.structs = NULL;
    ctx.struct_count = 0;
    ctx.struct_capacity = 0;
    ctx.funcs = NULL;
    ctx.func_count = 0;
    ctx.func_capacity = 0;
    ctx.enums = NULL;
    ctx.enum_count = 0;
    ctx.enum_capacity = 0;
    Environment global_env;
    env_init(&global_env, NULL);
    ctx.current_env = &global_env;
    // M22: install reserved prelude builtins.  These names cannot be
    // redefined by user code (reserved-prelude policy).
    {
        static const char *prelude_names[] = {
            "print", "len", "some", "ok", "err",
            "cli_arg_count", "cli_arg",
            "vec_push", "vec_pop", "vec_len", "vec_get", "vec_set",
            "map_get", "map_set", "map_has", "map_len", "map_remove",
            "str_buf_push", "str_buf_len", "str_buf_get", "str_buf_clear",
            NULL
        };
        for (int pi = 0; prelude_names[pi]; pi++) {
            Token pt;
            pt.start = prelude_names[pi];
            pt.length = (int)strlen(prelude_names[pi]);
            pt.line = 0;
            pt.kind = TOK_IDENT;
            pt.comment_start = NULL;
            pt.comment_length = 0;
            env_define_reserved(&global_env, pt, ty(&ctx, TYPE_INT));
        }
    }
    for (int m = 0; m < mod_count; m++) {
        ctx.path = mods[m].path;
        // M15: detect std/ library modules by path prefix.
        const char *p = mods[m].path;
        ctx.is_std = (strncmp(p, "std/", 4) == 0) ||
                     (p[0] == '/' && strstr(p, "/std/") != NULL) ||
                     (strstr(p, "./std/") != NULL);
        for (int i = 0; i < mods[m].count; i++) {
            check_node(&ctx, mods[m].stmts[i]);
        }
    }
    env_free(&global_env);
    // Free struct registry
    for (int i = 0; i < ctx.struct_count; i++) {
        free(ctx.structs[i].name);
    }
    free(ctx.structs);
    // M9.1: free function registry
    for (int i = 0; i < ctx.func_count; i++) {
        free(ctx.funcs[i].name);
    }
    free(ctx.funcs);
    // M13.1-P2: free enum registry
    for (int i = 0; i < ctx.enum_count; i++) {
        for (int j = 0; j < ctx.enums[i].variant_count; j++) {
            free(ctx.enums[i].variants[j]);
        }
        free(ctx.enums[i].variants);
        free(ctx.enums[i].name);
    }
    free(ctx.enums);
}
