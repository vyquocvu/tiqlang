// Environment and scope primitives for the semantic checker.
// Split from the monolithic src/semantic.c; see include/semantic_int.h.
#include "../include/semantic_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die_oom(void) {
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
bool env_define_reserved(Environment *env, Token name, SemanticType *type) {
    if (!env_define(env, name, false, type)) return false;
    env->symbols[env->count - 1].is_reserved = true;
    return true;
}

// M22: check whether a name collides with a reserved prelude builtin.
bool is_reserved_name(Environment *env, Token name) {
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

