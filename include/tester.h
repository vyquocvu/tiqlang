#ifndef TIQ_TESTER_H
#define TIQ_TESTER_H

#include <stdbool.h>

typedef struct {
    int passed;
    int failed;
    int skipped;
} TestResults;

void test_runner_init(void);
void test_runner_shutdown(void);

int run_tests_in_dir(const char *dir_path, TestResults *results);
int run_tests_in_file(const char *file_path, TestResults *results);

const char *test_get_cache_path(void);

#endif
