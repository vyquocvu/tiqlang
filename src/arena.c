#include "../include/arena.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default block: typical sources fit in one block, and tiny inputs pay
// a single malloc. Oversized requests get a dedicated block.
#define ARENA_BLOCK_SIZE (64 * 1024)
#define ARENA_ALIGN alignof(max_align_t)

struct ArenaBlock {
    ArenaBlock *next;
    size_t used;
    size_t capacity;
    unsigned char *data; // separately malloc'd, so it is max-aligned
};

static size_t align_up(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

static ArenaBlock *block_new(size_t capacity) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock));
    unsigned char *data = block ? malloc(capacity) : NULL;
    if (!data) { fprintf(stderr, "out of memory\n"); exit(1); }
    block->next = NULL;
    block->used = 0;
    block->capacity = capacity;
    block->data = data;
    return block;
}

void arena_init(Arena *arena) {
    arena->head = NULL;
    arena->block_count = 0;
}

void *arena_alloc(Arena *arena, size_t size) {
    size = align_up(size ? size : 1);
    ArenaBlock *block = arena->head;
    if (!block || block->used + size > block->capacity) {
        size_t capacity = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;
        ArenaBlock *fresh = block_new(capacity);
        fresh->next = block;
        arena->head = fresh;
        arena->block_count++;
        block = fresh;
    }
    void *p = block->data + block->used;
    block->used += size;
    return p;
}

void *arena_realloc(Arena *arena, void *ptr, size_t old_size, size_t new_size) {
    if (!ptr) return arena_alloc(arena, new_size);
    if (new_size <= old_size) return ptr;
    ArenaBlock *block = arena->head;
    size_t old_aligned = align_up(old_size);
    size_t new_aligned = align_up(new_size);
    // Extend in place when ptr is the newest allocation and there is room.
    if (block && (unsigned char *)ptr + old_aligned == block->data + block->used &&
        block->used - old_aligned + new_aligned <= block->capacity) {
        block->used += new_aligned - old_aligned;
        return ptr;
    }
    void *fresh = arena_alloc(arena, new_size);
    memcpy(fresh, ptr, old_size);
    return fresh;
}

void arena_reset(Arena *arena) {
    if (!arena->head) return;
    ArenaBlock *block = arena->head->next;
    while (block) {
        ArenaBlock *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    arena->head->next = NULL;
    arena->head->used = 0;
    arena->block_count = 1;
}

void arena_free(Arena *arena) {
    ArenaBlock *block = arena->head;
    while (block) {
        ArenaBlock *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    arena->head = NULL;
    arena->block_count = 0;
}
