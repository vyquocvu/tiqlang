#ifndef TIQ_BENCHMARK_H
#define TIQ_BENCHMARK_H

#include <stdbool.h>

typedef struct {
    bool verbose;
    bool quiet;
    int iterations;
} BenchmarkOptions;

void benchmark_init_options(BenchmarkOptions *opts);
int benchmark_files(const char **paths, int path_count, BenchmarkOptions *opts);

#endif
