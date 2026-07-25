#include "../include/type.h"
#include <stdlib.h>
#include <stdio.h>

void type_pool_init(TypePool *pool) {
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

void type_pool_free(TypePool *pool) {
    for (int i = 0; i < pool->count; i++) free(pool->items[i]);
    free(pool->items);
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

static SemanticType *intern(TypePool *pool, PrimitiveType kind,
                            SemanticType *element, int length, int param_count) {
    for (int i = 0; i < pool->count; i++) {
        SemanticType *t = pool->items[i];
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
    if (pool->count + 1 > pool->capacity) {
        pool->capacity = pool->capacity < 16 ? 16 : pool->capacity * 2;
        pool->items = realloc(pool->items, sizeof(SemanticType *) * pool->capacity);
        if (!pool->items) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    pool->items[pool->count++] = t;
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
