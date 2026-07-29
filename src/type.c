#include "../include/type.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void type_pool_init(TypePool *pool) {
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

void type_pool_free(TypePool *pool) {
    for (int i = 0; i < pool->count; i++) {
        SemanticType *t = pool->items[i];
        if (t->struct_name) {
            free((char *)t->struct_name);
            for (int j = 0; j < t->field_count; j++) free((char *)t->field_names[j]);
            free((void *)t->field_names);
            free(t->field_types);
        }
        free(t);
    }
    free(pool->items);
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

static void pool_push(TypePool *pool, SemanticType *t) {
    if (pool->count + 1 > pool->capacity) {
        pool->capacity = pool->capacity < 16 ? 16 : pool->capacity * 2;
        pool->items = realloc(pool->items, sizeof(SemanticType *) * pool->capacity);
        if (!pool->items) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    pool->items[pool->count++] = t;
}

static SemanticType *intern(TypePool *pool, PrimitiveType kind,
                            SemanticType *element, int length, int param_count) {
    for (int i = 0; i < pool->count; i++) {
        SemanticType *t = pool->items[i];
        if (t->struct_name) continue; // named structs are nominal, never structural
        if (t->kind == kind && t->element_type == element &&
            t->array_length == length && t->param_count == param_count) {
            return t;
        }
    }
    SemanticType *t = calloc(1, sizeof(SemanticType));
    if (!t) { fprintf(stderr, "out of memory\n"); exit(1); }
    t->kind = kind;
    t->element_type = element;
    t->array_length = length;
    t->param_count = param_count;
    pool_push(pool, t);
    return t;
}

SemanticType *type_get(TypePool *pool, PrimitiveType kind) {
    return intern(pool, kind, NULL, 0, 0);
}

SemanticType *type_get_func(TypePool *pool, PrimitiveType ret, int param_count) {
    return intern(pool, ret, NULL, 0, param_count);
}

SemanticType *type_get_array(TypePool *pool, SemanticType *element, int length) {
    return intern(pool, TYPE_ARRAY, element, length, 0);
}

SemanticType *type_get_slice(TypePool *pool, SemanticType *element) {
    return intern(pool, TYPE_SLICE, element, 0, 0);
}

static char *copy_token_text(Token t) {
    char *s = malloc((size_t)t.length + 1);
    if (!s) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(s, t.start, (size_t)t.length);
    s[t.length] = '\0';
    return s;
}

SemanticType *type_get_struct(TypePool *pool, Token name,
                              const Token *field_names,
                              SemanticType **field_types,
                              int field_count) {
    for (int i = 0; i < pool->count; i++) {
        SemanticType *t = pool->items[i];
        if (t->struct_name &&
            (int)strlen(t->struct_name) == (int)name.length &&
            memcmp(t->struct_name, name.start, name.length) == 0) {
            return t; // nominal identity: the declaration-site name wins
        }
    }
    SemanticType *t = calloc(1, sizeof(SemanticType));
    if (!t) { fprintf(stderr, "out of memory\n"); exit(1); }
    t->kind = TYPE_STRUCT;
    t->struct_name = copy_token_text(name);
    t->field_count = field_count;
    if (field_count > 0) {
        const char **names = malloc(sizeof(char *) * (size_t)field_count);
        SemanticType **types = malloc(sizeof(SemanticType *) * (size_t)field_count);
        if (!names || !types) { fprintf(stderr, "out of memory\n"); exit(1); }
        for (int i = 0; i < field_count; i++) {
            names[i] = copy_token_text(field_names[i]);
            types[i] = field_types[i];
        }
        t->field_names = names;
        t->field_types = types;
    }
    pool_push(pool, t);
    return t;
}

// M8: Option/Result type constructors.
SemanticType *type_get_option(TypePool *pool, SemanticType *inner) {
    // Structural interning: same inner type -> same Option instance.
    for (int i = 0; i < pool->count; i++) {
        SemanticType *t = pool->items[i];
        if (t->kind == TYPE_OPTION && t->inner_type == inner) {
            return t;
        }
    }
    SemanticType *t = calloc(1, sizeof(SemanticType));
    if (!t) { fprintf(stderr, "out of memory\n"); exit(1); }
    t->kind = TYPE_OPTION;
    t->inner_type = inner;
    pool_push(pool, t);
    return t;
}

SemanticType *type_get_result(TypePool *pool, SemanticType *inner, SemanticType *error) {
    // Structural interning: same inner+error types -> same Result instance.
    for (int i = 0; i < pool->count; i++) {
        SemanticType *t = pool->items[i];
        if (t->kind == TYPE_RESULT && t->inner_type == inner && t->error_type == error) {
            return t;
        }
    }
    SemanticType *t = calloc(1, sizeof(SemanticType));
    if (!t) { fprintf(stderr, "out of memory\n"); exit(1); }
    t->kind = TYPE_RESULT;
    t->inner_type = inner;
    t->error_type = error;
    pool_push(pool, t);
    return t;
}

static const char *kind_display_name(PrimitiveType kind) {
    switch (kind) {
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_STR: return "str";
        case TYPE_BOOL: return "bool";
        case TYPE_STR_VIEW: return "str_view";
        case TYPE_STREAM: return "stream";
        case TYPE_STRUCT: return "struct";
        case TYPE_OPTION: return "option";
        case TYPE_RESULT: return "result";
        case TYPE_REF: return "ref";
        case TYPE_REF_MUT: return "ref mut";
        case TYPE_CHAN: return "chan";
        case TYPE_UNIT: return "unit";
        case TYPE_I8: return "i8";
        case TYPE_I16: return "i16";
        case TYPE_I32: return "i32";
        case TYPE_U8: return "u8";
        case TYPE_U16: return "u16";
        case TYPE_U32: return "u32";
        case TYPE_U64: return "u64";
        case TYPE_F32: return "f32";
        case TYPE_NEVER: return "never";
        default: return "unknown";
    }
}

void type_display(const SemanticType *t, char *buf, size_t cap) {
    if (cap == 0) return;
    if (!t) { buf[0] = '\0'; return; }
    if (t->kind == TYPE_ARRAY) {
        char elem[96] = "";
        type_display(t->element_type, elem, sizeof elem);
        snprintf(buf, cap, "[%d]%s", t->array_length,
                 elem[0] ? elem : "unknown");
    } else if (t->kind == TYPE_SLICE) {
        char elem[96] = "";
        type_display(t->element_type, elem, sizeof elem);
        snprintf(buf, cap, "[]%s", elem[0] ? elem : "unknown");
    } else if (t->kind == TYPE_STRUCT && t->struct_name && t->struct_name[0]) {
        snprintf(buf, cap, "%s", t->struct_name);
    } else if (t->kind == TYPE_OPTION) {
        char inner[96] = "";
        type_display(t->inner_type, inner, sizeof inner);
        snprintf(buf, cap, "%s?", inner[0] ? inner : "unknown");
    } else if (t->kind == TYPE_RESULT) {
        char inner[96] = "";
        char error[96] = "";
        type_display(t->inner_type, inner, sizeof inner);
        type_display(t->error_type, error, sizeof error);
        snprintf(buf, cap, "%s!%s", inner[0] ? inner : "unknown", error[0] ? error : "unknown");
    } else {
        snprintf(buf, cap, "%s", kind_display_name(t->kind));
    }
}
