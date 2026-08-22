#ifndef TIQ_SEMANTIC_INT_H
#define TIQ_SEMANTIC_INT_H

// Internal semantic-checker API shared across the checker modules
// (src/env.c, src/symtab.c, src/typecheck.c, src/check_builtins.c,
// src/check_call.c, src/check_pattern.c, src/semantic.c). The public
// surface remains include/semantic.h; this header exists only so the
// monolithic src/semantic.c could be split into focused files without
// behavior change.

#include "semantic.h"
#include "type.h"

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
    // Pre-M13 S4: propagation tracking
    bool in_function;
    SemanticType *current_fn_return_type;
} SemanticContext;

// src/env.c
void die_oom(void);
bool env_define_reserved(Environment *env, Token name, SemanticType *type);
bool is_reserved_name(Environment *env, Token name);

// src/symtab.c
SemanticType *ty(SemanticContext *ctx, PrimitiveType kind);
SemanticType *resolve_type_annot(SemanticContext *ctx, Token tok);
SemanticType *struct_lookup(SemanticContext *ctx, Token name);
void struct_register(SemanticContext *ctx, const char *name, SemanticType *type);
EnumEntry *enum_lookup(SemanticContext *ctx, Token name);
int enum_variant_index(EnumEntry *e, Token variant);
void enum_register(SemanticContext *ctx, Token name, Token *variants, int variant_count);
AstNode *func_lookup(SemanticContext *ctx, Token name);
void func_register(SemanticContext *ctx, Token name, AstNode *node);

// src/typecheck.c
SemanticType *unify(SemanticContext *ctx, int line,
                    SemanticType *expected, SemanticType *found,
                    const char *context);
bool eq_comparable_kind(PrimitiveType k);
bool ffi_safe_kind(PrimitiveType k);

// src/check_builtins.c
bool vec_elem_type_ok(SemanticType *t);
bool vec_elem_same(SemanticType *expected, SemanticType *found);
void vec_unify_elem(SemanticContext *ctx, int line, SemanticType *expected,
                    SemanticType *found, const char *context);
SemanticType *resolve_container_annot(SemanticContext *ctx, Token tok, Token elem_tok);
void check_vec_builtin(SemanticContext *ctx, AstNode *node, Token name);
void check_strbuf_builtin(SemanticContext *ctx, AstNode *node, Token name);
void check_map_builtin(SemanticContext *ctx, AstNode *node, Token name);

// src/check_call.c
void check_call(SemanticContext *ctx, AstNode *node);

// src/check_pattern.c
void check_pattern(SemanticContext *ctx, Pattern *pat, SemanticType *scrutinee_type, Environment *arm_env);

// src/semantic.c
void check_node(SemanticContext *ctx, AstNode *node);

#endif
