#ifndef TIQ_TYPE_H
#define TIQ_TYPE_H

#include "semantic.h"

// Interned type pool. Structurally identical types share one canonical
// SemanticType instance, so pointer equality implies type equality.
// Pooled types are immutable after interning; inference replaces
// pointers instead of mutating type objects in place.
struct TypePool {
    SemanticType **items;
    int count;
    int capacity;
};

void type_pool_init(TypePool *pool);
void type_pool_free(TypePool *pool);

SemanticType *type_get(TypePool *pool, PrimitiveType kind);
SemanticType *type_get_func(TypePool *pool, PrimitiveType ret, int param_count);
SemanticType *type_get_array(TypePool *pool, SemanticType *element, int length);
SemanticType *type_get_slice(TypePool *pool, SemanticType *element);

#endif
