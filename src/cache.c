#define _POSIX_C_SOURCE 200809L
#include "../include/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#ifndef DT_REG
#define DT_REG 8
#endif

static int ensure_cache_dir(const Cache *cache) {
    if (!cache->initialized) return -1;
    if (mkdir(cache->dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

// Flatten a source path into a single cache file name: every path
// separator becomes '_', so entries always land inside the cache dir.
bool cache_entry_path(const Cache *cache, const char *source_path, char *buf, size_t cap) {
    if (!cache->initialized) return false;
    size_t dir_len = strlen(cache->dir);
    size_t key_len = strlen(source_path);
    // dir + '/' + key + ".c" + NUL must fit: fail closed, never truncate.
    if (dir_len + 1 + key_len + 2 + 1 > cap) return false;
    memcpy(buf, cache->dir, dir_len);
    buf[dir_len] = '/';
    for (size_t i = 0; i < key_len; i++) {
        char c = source_path[i];
        buf[dir_len + 1 + i] = (c == '/') ? '_' : c;
    }
    memcpy(buf + dir_len + 1 + key_len, ".c", 3);
    return true;
}

int cache_init(Cache *cache, const char *cache_dir_arg) {
    cache->initialized = false;
    cache->manifest_path[0] = '\0';
    const char *dir = cache_dir_arg;
    if (dir == NULL || *dir == '\0') {
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) {
            snprintf(cache->dir, sizeof(cache->dir), "%s/tiq", xdg);
        } else {
            const char *home = getenv("HOME");
            if (home && *home) {
                snprintf(cache->dir, sizeof(cache->dir), "%s/.cache/tiq", home);
            } else {
                snprintf(cache->dir, sizeof(cache->dir), "/tmp/.tiq-cache");
            }
        }
    } else {
        snprintf(cache->dir, sizeof(cache->dir), "%s", dir);
    }
    cache->initialized = true;
    return ensure_cache_dir(cache);
}

const char *cache_get_path(const Cache *cache) {
    return cache->dir;
}

bool cache_has(Cache *cache, const char *source_path, const char *c_path) {
    (void)c_path; // TODO: implement proper cache with c file path
    if (!cache->initialized) return false;
    if (ensure_cache_dir(cache) != 0) return false;

    char cached[1024];
    if (!cache_entry_path(cache, source_path, cached, sizeof(cached))) return false;
    FILE *f = fopen(cached, "rb");
    if (!f) return false;

    // Check if the cached file exists and is newer than source
    struct stat src_stat, cache_stat;
    if (stat(source_path, &src_stat) != 0) {
        fclose(f);
        return false;
    }
    if (fstat(fileno(f), &cache_stat) != 0) {
        fclose(f);
        return false;
    }

    // Check mtime - cache is valid if source hasn't changed
    if (src_stat.st_mtime > cache_stat.st_mtime) {
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}

void cache_put(Cache *cache, const char *source_path, const char *c_path) {
    if (!cache->initialized) return;
    if (ensure_cache_dir(cache) != 0) return;

    char cached[1024];
    if (!cache_entry_path(cache, source_path, cached, sizeof(cached))) return;

    // Copy c_path to cached location
    FILE *src = fopen(c_path, "rb");
    if (!src) return;
    FILE *dst = fopen(cached, "wb");
    if (!dst) {
        fclose(src);
        return;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            remove(cached);
            return;
        }
    }

    fclose(src);
    fclose(dst);
}

void cache_remove(Cache *cache, const char *source_path) {
    if (!cache->initialized) return;
    char cached[1024];
    if (!cache_entry_path(cache, source_path, cached, sizeof(cached))) return;
    remove(cached);
}

void cache_clear(Cache *cache) {
    if (!cache->initialized) return;
    DIR *dir = opendir(cache->dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", cache->dir, entry->d_name);
            remove(path);
        }
    }
    closedir(dir);
}

int cache_load_manifest(Cache *cache, const char *manifest_path_arg) {
    if (manifest_path_arg) {
        snprintf(cache->manifest_path, sizeof(cache->manifest_path), "%s", manifest_path_arg);
        return 0;
    }
    snprintf(cache->manifest_path, sizeof(cache->manifest_path), "%s/manifest.json", cache->dir);
    FILE *f = fopen(cache->manifest_path, "r");
    if (!f) return -1;
    fclose(f);
    return 0;
}

int cache_save_manifest(Cache *cache, const char *manifest_path_arg) {
    if (manifest_path_arg) {
        snprintf(cache->manifest_path, sizeof(cache->manifest_path), "%s", manifest_path_arg);
    }
    return 0;
}
