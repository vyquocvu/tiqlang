#define _POSIX_C_SOURCE 200809L
#include "../include/tester.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static char test_cache_dir[1024] = {0};

void test_runner_init(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(test_cache_dir, sizeof(test_cache_dir), "%s/tiq-tests", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(test_cache_dir, sizeof(test_cache_dir), "%s/.cache/tiq-tests", home);
        } else {
            snprintf(test_cache_dir, sizeof(test_cache_dir), "/tmp/.tiq-test-cache");
        }
    }
    mkdir(test_cache_dir, 0755);
}

void test_runner_shutdown(void) {
    // Nothing to clean up
}

const char *test_get_cache_path(void) {
    return test_cache_dir;
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0) { fclose(file); return NULL; }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *data = malloc((size_t)size + 1);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) { free(data); fclose(file); return NULL; }
    data[size] = '\0';
    fclose(file);
    return data;
}

static char *temporary_c_template(void) {
    const char *dir = getenv("TMPDIR");
    const char *suffix = "tiq-test-c-XXXXXX";
    size_t dir_len, suffix_len = strlen(suffix);
    int need_slash;
    char *path;
    if (dir == NULL || *dir == '\0') dir = "/tmp";
    dir_len = strlen(dir);
    need_slash = dir_len > 0U && dir[dir_len - 1U] != '/';
    path = malloc(dir_len + (need_slash ? 1U : 0U) + suffix_len + 1U);
    if (path == NULL) return NULL;
    memcpy(path, dir, dir_len);
    if (need_slash) { path[dir_len] = '/'; dir_len++; }
    memcpy(path + dir_len, suffix, suffix_len + 1U);
    return path;
}

static int compile_and_run(const char *source_path, const char *expected_output, TestResults *results) {
    // Read source
    char *source = read_all(source_path);
    if (!source) {
        fprintf(stderr, "tiq: cannot read %s\n", source_path);
        return -1;
    }

    // Create temp C file
    char *temp_name = temporary_c_template();
    if (!temp_name) {
        free(source);
        return -1;
    }
    int temp_fd = mkstemp(temp_name);
    if (temp_fd < 0) {
        free(source);
        free(temp_name);
        return -1;
    }
    FILE *temp_file = fdopen(temp_fd, "wb");
    if (!temp_file) {
        close(temp_fd);
        remove(temp_name);
        free(source);
        free(temp_name);
        return -1;
    }

    // Parse and check
    DiagContext diag;
    diag_init(&diag);
    Parser parser;
    parser_init(&parser, source, source_path, &diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    if (diag.has_error) {
        fprintf(stderr, "tiq: parse error in %s\n", source_path);
        free(stmts);
        parser_free(&parser);
        free(source);
        fclose(temp_file);
        remove(temp_name);
        free(temp_name);
        return -1;
    }
    semantic_check(stmts, count, source_path, &diag);
    if (diag.has_error) {
        fprintf(stderr, "tiq: semantic error in %s\n", source_path);
        free(stmts);
        parser_free(&parser);
        free(source);
        fclose(temp_file);
        remove(temp_name);
        free(temp_name);
        return -1;
    }

    // We need to compile to C - for tests, we use the main compiler
    // This is a simplified version; in production would reuse compile_to_c
    fclose(temp_file);
    remove(temp_name);
    free(temp_name);
    free(stmts);
    parser_free(&parser);
    free(source);

    // Use the main compiler to build and run
    char *exe_path = malloc(strlen(test_cache_dir) + 64);
    sprintf(exe_path, "%s/test-%d", test_cache_dir, (int)(intptr_t)source_path % 100000);

    // Build using host compiler
    pid_t pid = fork();
    if (pid < 0) {
        free(exe_path);
        return -1;
    }
    if (pid == 0) {
        // Child: use tiq build
        char *tiq_path = "./build/tiq";
        char *args[] = { tiq_path, "build", (char*)source_path, "-o", exe_path, NULL };
        execvp(tiq_path, args);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(exe_path);
        return -1;
    }

    // Run the executable
    FILE *out = popen(exe_path, "r");
    if (!out) {
        remove(exe_path);
        free(exe_path);
        return -1;
    }

    char output[4096] = {0};
    size_t out_len = 0;
    int ch;
    while ((ch = fgetc(out)) != EOF && out_len < sizeof(output) - 1) {
        output[out_len++] = (char)ch;
    }
    output[out_len] = '\0';
    int pclose_result = pclose(out);
    (void)pclose_result; // Reserved for future use

    // Compare output
    int pass = (out_len == strlen(expected_output)) &&
               (memcmp(output, expected_output, out_len) == 0);

    remove(exe_path);
    free(exe_path);

    if (pass) {
        results->passed++;
        return 0;
    } else {
        results->failed++;
        fprintf(stderr, "FAIL: %s\n  expected: %s\n  got: %s\n",
                source_path, expected_output, output);
        return -1;
    }
}

static int run_test_file(const char *file_path, TestResults *results) {
    // Read the test file and look for test assertions
    // Format: // test: expected output
    //         code
    //         // output: expected

    char *source = read_all(file_path);
    if (!source) return 0;

    // Simple test: check for "//! expected" comments
    char *expected = NULL;
    char *p = source;
    while (*p) {
        if (p[0] == '/' && p[1] == '!' && p[2] == ' ') {
            // Found test output marker
            p += 3;
            char *start = p;
            while (*p && *p != '\n') p++;
            size_t len = (size_t)(p - start);
            expected = malloc(len + 1);
            memcpy(expected, start, len);
            expected[len] = '\0';
            break;
        }
        p++;
    }

    if (expected) {
        compile_and_run(file_path, expected, results);
        free(expected);
    }

    free(source);
    return 0;
}

int run_tests_in_file(const char *file_path, TestResults *results) {
    struct stat st;
    if (stat(file_path, &st) != 0) return -1;

    if (S_ISREG(st.st_mode)) {
        return run_test_file(file_path, results);
    }
    return 0;
}

int run_tests_in_dir(const char *dir_path, TestResults *results) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(dir)) != NULL) {
        // Skip hidden files and non-.tiq files
        if (entry->d_name[0] == '.') continue;

        size_t name_len = strlen(entry->d_name);
        if (name_len < 4 || strcmp(entry->d_name + name_len - 4, ".tiq") != 0) {
            continue;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISREG(st.st_mode)) {
            run_test_file(path, results);
            found++;
        } else if (S_ISDIR(st.st_mode)) {
            run_tests_in_dir(path, results);
        }
    }

    closedir(dir);
    return found > 0 ? 0 : -1;
}
