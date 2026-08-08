#ifndef TIQ_SEMANTIC_H
#define TIQ_SEMANTIC_H

#include "ast.h"
#include "diag.h"

typedef enum {
    TYPE_UNKNOWN,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_BOOL,
    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_STR_VIEW,
    TYPE_STREAM,
    TYPE_STRUCT,
    TYPE_OPTION,
    TYPE_RESULT,
    TYPE_REF,
    TYPE_REF_MUT,
    TYPE_CHAN,
    TYPE_UNIT,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_F32,
    TYPE_NEVER,
    // M13.1-P3: growable array handle (LANGUAGE_SPEC §19.7); element type
    // lives in element_type, NULL until the first vec_push establishes it.
    TYPE_VEC,
    // M13.1-P4: growable string buffer handle (LANGUAGE_SPEC §19.8);
    // not parametrized — the contents are always bytes.
    TYPE_STRBUF,
    // M13.1-P5: insertion-ordered hash map handle (LANGUAGE_SPEC §19.9);
    // not parametrized — keys are always str and values always int.
    TYPE_MAP,
    // Canonical aliases: the inference defaults TYPE_INT/TYPE_FLOAT are
    // i64/f64 (LANGUAGE_SPEC §11); sharing values keeps pooled types unique.
    TYPE_I64 = TYPE_INT,
    TYPE_F64 = TYPE_FLOAT
} PrimitiveType;

typedef struct SemanticType {
    PrimitiveType kind;
    int param_count;
    struct SemanticType *element_type;
    int array_length;
    // Nominal struct metadata (plan 3.2/3.3): NULL for non-struct types.
    // The strings and arrays are owned by the TypePool, not the AST.
    const char *struct_name;
    int field_count;
    const char **field_names;
    struct SemanticType **field_types;
    // M8: Option/Result inner types
    struct SemanticType *inner_type;   // T for T? or T!E
    struct SemanticType *error_type;   // E for T!E (NULL for Option)
} SemanticType;

typedef struct Symbol {
    Token name;
    bool is_mutable;
    bool is_moved;
    bool is_reserved;  // M22: prelude builtin — user cannot redefine
    SemanticType *type;
} Symbol;

typedef struct Environment {
    struct Environment *parent;
    Symbol *symbols;
    int count;
    int capacity;
} Environment;

typedef struct TypePool TypePool;

void semantic_check(AstNode **stmts, int count, const char *path, DiagContext *diag, TypePool *pool);

// M13.1-P6: one module's top-level statements (imports already stripped)
// plus the path used for its diagnostics. A multi-file program is checked
// as the post-order module sequence over one shared registry set, so
// struct/enum registries and function symbols span modules (§17.6).
typedef struct {
    AstNode **stmts;
    int count;
    const char *path;
} SemanticModule;

void semantic_check_modules(SemanticModule *mods, int mod_count, DiagContext *diag, TypePool *pool);

// Environment primitives, exposed for the unit test harness (tests/unit/).
void env_init(Environment *env, Environment *parent);
void env_free(Environment *env);
bool env_define(Environment *env, Token name, bool is_mutable, SemanticType *type);
Symbol *env_lookup(Environment *env, Token name);

#endif
