// Symbol tables and struct/enum/function registries for the
// semantic checker. Split from the monolithic src/semantic.c.
#include "../include/semantic_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SemanticType *ty(SemanticContext *ctx, PrimitiveType kind) {
    return type_get(ctx->pool, kind);
}

// Forward declaration for struct registry lookup (defined below)
SemanticType *struct_lookup(SemanticContext *ctx, Token name);

// M12.4: Resolve a type annotation token to a SemanticType.
// Returns NULL if the token is not a valid type name.
SemanticType *resolve_type_annot(SemanticContext *ctx, Token tok) {
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
        {"Option", 6, TYPE_OPTION}, {"Result", 6, TYPE_RESULT},
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
SemanticType *struct_lookup(SemanticContext *ctx, Token name) {
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

void struct_register(SemanticContext *ctx, const char *name, SemanticType *type) {
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
EnumEntry *enum_lookup(SemanticContext *ctx, Token name) {
    for (int i = 0; i < ctx->enum_count; i++) {
        if ((int)strlen(ctx->enums[i].name) == (int)name.length &&
            memcmp(ctx->enums[i].name, name.start, name.length) == 0) {
            return &ctx->enums[i];
        }
    }
    return NULL;
}

// Returns the variant's declaration index (its value), or -1 if unknown.
int enum_variant_index(EnumEntry *e, Token variant) {
    for (int i = 0; i < e->variant_count; i++) {
        if ((int)strlen(e->variants[i]) == (int)variant.length &&
            memcmp(e->variants[i], variant.start, variant.length) == 0) {
            return i;
        }
    }
    return -1;
}

void enum_register(SemanticContext *ctx, Token name, Token *variants, int variant_count) {
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
AstNode *func_lookup(SemanticContext *ctx, Token name) {
    // Iterate backwards so the most recent definition wins.
    for (int i = ctx->func_count - 1; i >= 0; i--) {
        if ((int)strlen(ctx->funcs[i].name) == (int)name.length &&
            memcmp(ctx->funcs[i].name, name.start, name.length) == 0) {
            return ctx->funcs[i].node;
        }
    }
    return NULL;
}

void func_register(SemanticContext *ctx, Token name, AstNode *node) {
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
