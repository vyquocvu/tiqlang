#ifndef TIQ_CACHE_H
#define TIQ_CACHE_H

#include <stdbool.h>

typedef struct CacheEntry CacheEntry;

int cache_init(const char *cache_dir);
void cache_shutdown(void);
const char *cache_get_path(void);

bool cache_has(const char *source_path, const char *c_path);
void cache_put(const char *source_path, const char *c_path);
void cache_remove(const char *source_path);
void cache_clear(void);

int cache_load_manifest(const char *manifest_path);
int cache_save_manifest(const char *manifest_path);

#endif
