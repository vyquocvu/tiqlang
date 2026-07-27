#ifndef TIQ_CACHE_H
#define TIQ_CACHE_H

#include <stdbool.h>
#include <stddef.h>

// Caller-owned cache context (plan 5.3): no module-level statics, so two
// contexts never share state and path buffers are caller-provided.
typedef struct {
    char dir[1024];
    char manifest_path[1024];
    bool initialized;
} Cache;

// Resolves the cache directory (arg, else $XDG_CACHE_HOME/tiq, else
// $HOME/.cache/tiq, else /tmp/.tiq-cache) and creates it. Returns 0 on
// success, -1 on failure.
int cache_init(Cache *cache, const char *cache_dir);
const char *cache_get_path(const Cache *cache);

// Writes the on-disk artifact path for a source into a caller buffer.
// Returns false (fail closed) if the path would not fit in cap.
bool cache_entry_path(const Cache *cache, const char *source_path, char *buf, size_t cap);

bool cache_has(Cache *cache, const char *source_path, const char *c_path);
void cache_put(Cache *cache, const char *source_path, const char *c_path);
void cache_remove(Cache *cache, const char *source_path);
void cache_clear(Cache *cache);

int cache_load_manifest(Cache *cache, const char *manifest_path);
int cache_save_manifest(Cache *cache, const char *manifest_path);

#endif
