#ifndef TIQ_MANIFEST_H
#define TIQ_MANIFEST_H

#include <stdbool.h>

typedef enum {
    MANIFEST_OK = 0,
    MANIFEST_NOT_FOUND,
    MANIFEST_PARSE_ERROR,
    MANIFEST_INVALID_FORMAT
} ManifestStatus;

typedef struct {
    char *name;
    char *version;
    char *description;
    char **deps;
    int dep_count;
    char *license;
    char *repository;
    char **test_dirs;
    int test_dir_count;
    char **src_files;
    int src_file_count;
} Manifest;

void manifest_init(Manifest *m);
void manifest_free(Manifest *m);
ManifestStatus manifest_load(const char *path, Manifest *m);
int manifest_save(const char *path, const Manifest *m);

bool manifest_validate(const Manifest *m, char **error_out);

#endif
