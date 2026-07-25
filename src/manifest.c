#define _POSIX_C_SOURCE 200809L
#include "../include/manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void manifest_init(Manifest *m) {
    memset(m, 0, sizeof(*m));
}

void manifest_free(Manifest *m) {
    free(m->name);
    free(m->version);
    free(m->description);
    free(m->license);
    free(m->repository);
    for (int i = 0; i < m->dep_count; i++) {
        free(m->deps[i]);
    }
    free(m->deps);
    for (int i = 0; i < m->test_dir_count; i++) {
        free(m->test_dirs[i]);
    }
    free(m->test_dirs);
    for (int i = 0; i < m->src_file_count; i++) {
        free(m->src_files[i]);
    }
    free(m->src_files);
    memset(m, 0, sizeof(*m));
}

static char *copy_string(const char *s) {
    if (!s) return NULL;
    char *r = malloc(strlen(s) + 1);
    if (r) strcpy(r, s);
    return r;
}

static void add_string(char ***array, int *count, const char *value) {
    if (!value) return;
    *array = realloc(*array, ((*count) + 1) * sizeof(char*));
    (*array)[*count] = copy_string(value);
    (*count)++;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) *end-- = '\0';
    return s;
}

ManifestStatus manifest_load(const char *path, Manifest *m) {
    FILE *f = fopen(path, "r");
    if (!f) return MANIFEST_NOT_FOUND;

    char line[1024];
    int section = -1; // -1=none, 0=package, 1=deps, 2=tests

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);

        // Skip empty lines and comments
        if (!*trimmed || *trimmed == '#') continue;

        // Check for section headers
        if (strcmp(trimmed, "[package]") == 0) { section = 0; continue; }
        if (strcmp(trimmed, "[deps]") == 0) { section = 1; continue; }
        if (strcmp(trimmed, "[tests]") == 0) { section = 2; continue; }

        // Parse key=value pairs
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);

        // Remove quotes from values if present
        size_t val_len = strlen(value);
        if (val_len >= 2) {
            if ((value[0] == '"' && value[val_len-1] == '"') ||
                (value[0] == '\'' && value[val_len-1] == '\'')) {
                value[val_len-1] = '\0';
                value++;
            }
        }

        switch (section) {
            case 0: // package
                if (strcmp(key, "name") == 0) m->name = copy_string(value);
                else if (strcmp(key, "version") == 0) m->version = copy_string(value);
                else if (strcmp(key, "description") == 0) m->description = copy_string(value);
                else if (strcmp(key, "license") == 0) m->license = copy_string(value);
                else if (strcmp(key, "repository") == 0) m->repository = copy_string(value);
                else if (strcmp(key, "src") == 0) add_string(&m->src_files, &m->src_file_count, value);
                break;
            case 1: // deps
                add_string(&m->deps, &m->dep_count, value);
                break;
            case 2: // tests
                if (strcmp(key, "dir") == 0) add_string(&m->test_dirs, &m->test_dir_count, value);
                else if (strcmp(key, "include") == 0) add_string(&m->src_files, &m->src_file_count, value);
                break;
        }
    }

    fclose(f);
    return MANIFEST_OK;
}

int manifest_save(const char *path, const Manifest *m) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "[package]\n");
    if (m->name) fprintf(f, "name = \"%s\"\n", m->name);
    if (m->version) fprintf(f, "version = \"%s\"\n", m->version);
    if (m->description) fprintf(f, "description = \"%s\"\n", m->description);
    if (m->license) fprintf(f, "license = \"%s\"\n", m->license);
    if (m->repository) fprintf(f, "repository = \"%s\"\n", m->repository);
    for (int i = 0; i < m->src_file_count; i++) {
        fprintf(f, "src = \"%s\"\n", m->src_files[i]);
    }

    if (m->dep_count > 0) {
        fprintf(f, "\n[deps]\n");
        for (int i = 0; i < m->dep_count; i++) {
            fprintf(f, "%s\n", m->deps[i]);
        }
    }

    if (m->test_dir_count > 0) {
        fprintf(f, "\n[tests]\n");
        for (int i = 0; i < m->test_dir_count; i++) {
            fprintf(f, "dir = \"%s\"\n", m->test_dirs[i]);
        }
    }

    fclose(f);
    return 0;
}

bool manifest_validate(const Manifest *m, char **error_out) {
    if (!m->name || !*m->name) {
        if (error_out) *error_out = copy_string("package name is required");
        return false;
    }

    // Version format check (simple: major.minor.patch)
    if (m->version) {
        char *v = m->version;
        int dots = 0;
        while (*v) {
            if (*v == '.') dots++;
            else if (!isdigit((unsigned char)*v)) {
                if (error_out) *error_out = copy_string("version must be numeric (major.minor.patch)");
                return false;
            }
            v++;
        }
        if (dots != 2) {
            if (error_out) *error_out = copy_string("version must be in format major.minor.patch");
            return false;
        }
    }

    return true;
}
