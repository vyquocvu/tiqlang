#ifndef TIQ_ARENA_H
#define TIQ_ARENA_H

#include <stddef.h>

// Bump allocator for AST nodes and node-owned aux arrays (plan 4.1).
// Individual allocations are never freed; the whole arena is released
// in one arena_free, which removes the parser's per-node partial-free
// bookkeeping and the entire class of partial-free leaks.

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;   // newest block; allocation always happens here
    int block_count;    // observable for the growth unit tests
} Arena;

void arena_init(Arena *arena);
// Returns memory aligned for any object type; aborts on OOM (matching
// the parser's existing out-of-memory policy).
void *arena_alloc(Arena *arena, size_t size);
// realloc-style grow: extends the newest allocation in place when
// possible, otherwise copies old_size bytes into fresh arena memory.
// The abandoned region stays in the arena until arena_free.
void *arena_realloc(Arena *arena, void *ptr, size_t old_size, size_t new_size);
// Rewind to empty for reuse: keeps the newest block, frees the rest.
void arena_reset(Arena *arena);
void arena_free(Arena *arena);

#endif
