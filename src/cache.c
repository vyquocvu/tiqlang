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

static char cache_dir[1024] = {0};
static bool cache_initialized = false;

static int ensure_cache_dir(void) {
    if (!cache_initialized) return -1;
    if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static char *get_source_key(const char *source_path) {
    // Simple hash: just use the absolute path for now
    // In production, would use file mtime and content hash
    static char key[256];
    snprintf(key, sizeof(key), "%s", source_path);
    return key;
}

static char *get_cache_entry_path(const char *source_path) {
    static char path[1024];
    char *key = get_source_key(source_path);
    snprintf(path, sizeof(path), "%s/%s.c", cache_dir, key);
    return path;
}

int cache_init(const char *cache_dir_arg) {
    const char *dir = cache_dir_arg;
    if (dir == NULL || *dir == '\0') {
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) {
            snprintf(cache_dir, sizeof(cache_dir), "%s/tiq", xdg);
        } else {
            const char *home = getenv("HOME");
            if (home && *home) {
                snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/tiq", home);
            } else {
                snprintf(cache_dir, sizeof(cache_dir), "/tmp/.tiq-cache");
            }
        }
    } else {
        snprintf(cache_dir, sizeof(cache_dir), "%s", dir);
    }
    cache_initialized = true;
    return ensure_cache_dir();
}

void cache_shutdown(void) {
    // Nothing to clean up for now
}

const char *cache_get_path(void) {
    return cache_dir;
}

bool cache_has(const char *source_path, const char *c_path) {
    (void)c_path; // TODO: implement proper cache with c file path
    if (!cache_initialized) return false;
    if (ensure_cache_dir() != 0) return false;

    char *cached = get_cache_entry_path(source_path);
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

void cache_put(const char *source_path, const char *c_path) {
    if (!cache_initialized) return;
    if (ensure_cache_dir() != 0) return;

    char *cached = get_cache_entry_path(source_path);

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

void cache_remove(const char *source_path) {
    if (!cache_initialized) return;
    char *cached = get_cache_entry_path(source_path);
    remove(cached);
}

void cache_clear(void) {
    if (!cache_initialized) return;
    DIR *dir = opendir(cache_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", cache_dir, entry->d_name);
            remove(path);
        }
    }
    closedir(dir);
}

static char manifest_path[1024] = {0};

int cache_load_manifest(const char *manifest_path_arg) {
    if (manifest_path_arg) {
        snprintf(manifest_path, sizeof(manifest_path), "%s", manifest_path_arg);
        return 0;
    }
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", cache_dir);
    FILE *f = fopen(manifest_path, "r");
    if (!f) return -1;
    fclose(f);
    return 0;
}

int cache_save_manifest(const char *manifest_path_arg) {
    if (manifest_path_arg) {
        snprintf(manifest_path, sizeof(manifest_path), "%s", manifest_path_arg);
    }
    return 0;
}
