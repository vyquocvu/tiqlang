#ifndef TIQ_TYPE_H
#define TIQ_TYPE_H

#include "semantic.h"
#include <stddef.h>

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

// Nominal struct interning (plan 3.2/3.3): identity is the declared name,
// so the same name always returns the same instance regardless of fields.
// Field name strings and the type array are copied into pool ownership.
SemanticType *type_get_struct(TypePool *pool, Token name,
                              const Token *field_names,
                              SemanticType **field_types,
                              int field_count);

// M8: Option/Result type constructors.
// type_get_option(pool, T) returns the canonical T? type.
// type_get_result(pool, T, E) returns the canonical T!E type.
SemanticType *type_get_option(TypePool *pool, SemanticType *inner);
SemanticType *type_get_result(TypePool *pool, SemanticType *inner, SemanticType *error);

// User-facing type name for diagnostics ("expected <T>, found <U>"),
// e.g. "int", "str", "[3]int", "[]int". The parser keeps its own
// TYPE_* dump format for golden ASTs.
void type_display(const SemanticType *t, char *buf, size_t cap);

#endif
