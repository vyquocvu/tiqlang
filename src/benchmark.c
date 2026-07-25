#define _POSIX_C_SOURCE 200809L
#include "../include/benchmark.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

void benchmark_init_options(BenchmarkOptions *opts) {
    opts->verbose = false;
    opts->quiet = false;
    opts->iterations = 1;
}

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

typedef struct {
    const char *name;
    double lexer_time;
    double parse_time;
    double semantic_time;
    double total_time;
    int file_size;
} BenchmarkResult;

static char *read_all(const char *path, long *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *data = malloc((size_t)size + 1);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return NULL; }
    data[size] = '\0';
    fclose(f);
    if (size_out) *size_out = size;
    return data;
}

static void benchmark_file(const char *path, BenchmarkResult *result) {
    long file_size = 0;
    char *source = read_all(path, &file_size);
    if (!source) {
        fprintf(stderr, "tiq: cannot read %s\n", path);
        return;
    }

    result->name = path;
    result->file_size = (int)file_size;

    DiagContext diag;
    double start, end;

    // Benchmark lexer
    diag_init(&diag);
    start = get_time_ms();
    {
        Lexer lexer;
        lexer_init(&lexer, source, path, &diag);
        Token token;
        do {
            token = lexer_next(&lexer);
        } while (token.kind != TOK_EOF);
    }
    end = get_time_ms();
    result->lexer_time = end - start;

    // Benchmark parser
    diag_init(&diag);
    start = get_time_ms();
    {
        Parser parser;
        parser_init(&parser, source, path, &diag);
        int stmt_count;
        AstNode **stmts = parser_parse(&parser, &stmt_count);
        for (int i = 0; i < stmt_count; i++) {
            // Walk the AST to ensure full parsing
            (void)stmts[i];
        }
        free(stmts);
        parser_free(&parser);
    }
    end = get_time_ms();
    result->parse_time = end - start;

    // Benchmark semantic check
    diag_init(&diag);
    start = get_time_ms();
    {
        Parser parser;
        parser_init(&parser, source, path, &diag);
        int stmt_count;
        AstNode **stmts = parser_parse(&parser, &stmt_count);
        TypePool pool;
        type_pool_init(&pool);
        if (!diag.has_error) {
            semantic_check(stmts, stmt_count, path, &diag, &pool);
        }
        free(stmts);
        parser_free(&parser);
        type_pool_free(&pool);
    }
    end = get_time_ms();
    result->semantic_time = end - start;

    result->total_time = result->lexer_time + result->parse_time + result->semantic_time;

    free(source);
}

static void benchmark_file_repeatedly(const char *path, int iterations, BenchmarkResult *result) {
    long file_size = 0;
    char *source = read_all(path, &file_size);
    if (!source) {
        fprintf(stderr, "tiq: cannot read %s\n", path);
        return;
    }

    result->name = path;
    result->file_size = (int)file_size;
    result->lexer_time = 0;
    result->parse_time = 0;
    result->semantic_time = 0;

    for (int iter = 0; iter < iterations; iter++) {
        DiagContext diag;

        // Lexer
        diag_init(&diag);
        double start = get_time_ms();
        {
            Lexer lexer;
            lexer_init(&lexer, source, path, &diag);
            Token token;
            do { token = lexer_next(&lexer); } while (token.kind != TOK_EOF);
        }
        result->lexer_time += get_time_ms() - start;

        // Parser
        diag_init(&diag);
        start = get_time_ms();
        {
            Parser parser;
            parser_init(&parser, source, path, &diag);
            int stmt_cnt;
            AstNode **stmts = parser_parse(&parser, &stmt_cnt);
            (void)stmt_cnt;
            free(stmts);
            parser_free(&parser);
        }
        result->parse_time += get_time_ms() - start;

        // Semantic
        diag_init(&diag);
        start = get_time_ms();
        {
            Parser parser;
            parser_init(&parser, source, path, &diag);
            int stmt_cnt;
            AstNode **stmts = parser_parse(&parser, &stmt_cnt);
            TypePool pool;
            type_pool_init(&pool);
            if (!diag.has_error) {
                semantic_check(stmts, stmt_cnt, path, &diag, &pool);
            }
            free(stmts);
            parser_free(&parser);
            type_pool_free(&pool);
        }
        result->semantic_time += get_time_ms() - start;
    }

    // Average times
    result->lexer_time /= iterations;
    result->parse_time /= iterations;
    result->semantic_time /= iterations;
    result->total_time = result->lexer_time + result->parse_time + result->semantic_time;

    free(source);
}

static void collect_files(const char *dir, char ***files, int *count, int *capacity) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(dir) + strlen(entry->d_name) + 2;
        char *path = malloc(len);
        snprintf(path, len, "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISREG(st.st_mode)) {
                size_t name_len = strlen(entry->d_name);
                if (name_len > 4 && strcmp(entry->d_name + name_len - 4, ".tiq") == 0) {
                    if (*count >= *capacity) {
                        *capacity = *capacity == 0 ? 64 : *capacity * 2;
                        *files = realloc(*files, (size_t)*capacity * sizeof(char*));
                    }
                    (*files)[(*count)++] = path;
                    continue;
                }
            } else if (S_ISDIR(st.st_mode)) {
                collect_files(path, files, count, capacity);
                free(path);
                continue;
            }
        }
        free(path);
    }
    closedir(d);
}

static void print_result(BenchmarkResult *r, bool verbose) {
    if (verbose) {
        printf("%.*s:\n", (int)strlen(r->name), r->name);
        printf("  file size:    %d bytes\n", r->file_size);
        printf("  lexer:        %.3f ms\n", r->lexer_time);
        printf("  parser:       %.3f ms\n", r->parse_time);
        printf("  semantic:     %.3f ms\n", r->semantic_time);
        printf("  total:        %.3f ms\n", r->total_time);
        if (r->file_size > 0) {
            printf("  throughput:   %.0f bytes/s\n", 
                   (double)r->file_size / (r->total_time / 1000.0));
        }
    } else {
        printf("%-40.*s %8.2f ms\n", (int)strlen(r->name), r->name, r->total_time);
    }
}

int benchmark_files(const char **paths, int path_count, BenchmarkOptions *opts) {
    BenchmarkResult results[256];
    int result_count = 0;
    double total_time = 0;

    for (int i = 0; i < path_count; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            char **files = NULL;
            int count = 0, capacity = 0;
            collect_files(paths[i], &files, &count, &capacity);

            for (int j = 0; j < count; j++) {
                if (result_count < 256) {
                    if (opts->iterations > 1) {
                        benchmark_file_repeatedly(files[j], opts->iterations, &results[result_count]);
                    } else {
                        benchmark_file(files[j], &results[result_count]);
                    }
                    total_time += results[result_count].total_time;
                    result_count++;
                }
                free(files[j]);
            }
            free(files);
        } else if (stat(paths[i], &st) == 0) {
            if (result_count < 256) {
                if (opts->iterations > 1) {
                    benchmark_file_repeatedly(paths[i], opts->iterations, &results[result_count]);
                } else {
                    benchmark_file(paths[i], &results[result_count]);
                }
                total_time += results[result_count].total_time;
                result_count++;
            }
        }
    }

    if (result_count == 0) {
        printf("No .tiq files found to benchmark.\n");
        return 1;
    }

    // Print results
    if (!opts->quiet) {
        printf("\n=== Benchmark Results ===\n\n");
    }

    if (!opts->verbose && !opts->quiet) {
        printf("%-40s %8s\n", "File", "Total (ms)");
        printf("%-40s %8s\n", "----", "----------");
    }

    double min_time = results[0].total_time;
    double max_time = results[0].total_time;
    double avg_time = 0;

    for (int i = 0; i < result_count; i++) {
        if (!opts->quiet) {
            print_result(&results[i], opts->verbose);
        }
        if (results[i].total_time < min_time) min_time = results[i].total_time;
        if (results[i].total_time > max_time) max_time = results[i].total_time;
        avg_time += results[i].total_time;
    }
    avg_time /= result_count;

    // Always print summary
    if (opts->quiet) {
        printf("Files: %d, Avg: %.3f ms, Total: %.3f ms\n", result_count, avg_time, total_time);
    } else {
        printf("\n=== Summary ===\n");
        printf("Files:      %d\n", result_count);
        printf("Min time:   %.3f ms\n", min_time);
        printf("Max time:   %.3f ms\n", max_time);
        printf("Avg time:   %.3f ms\n", avg_time);
        printf("Total time: %.3f ms\n", total_time);
    }

    return 0;
}
