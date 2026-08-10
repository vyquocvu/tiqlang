// M17.2: QBE backend runtime library.
// All functions have external linkage so QBE-generated object files
// can call them. This replaces the static-inline runtime that the
// C backend embeds in every generated translation unit.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// --- Memory ----------------------------------------------------------------

void *tiq_alloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "tiq: out of memory\n"); exit(1); }
    return p;
}

const char *tiq_str_dup(const char *s) {
    size_t n = strlen(s);
    char *b = (char *)tiq_alloc(n + 1);
    memcpy(b, s, n + 1);
    return b;
}

// --- Print (type-dispatched) -----------------------------------------------
// The QBE backend emits calls to these based on the IR type of the
// print argument. Each prints the value followed by a newline, matching
// the C backend's printf-based emission.

void tiq_print_i64(int64_t v) {
    printf("%lld\n", (long long)v);
}

void tiq_print_str(const char *s) {
    printf("%s\n", s ? s : "");
}

void tiq_print_bool(int64_t v) {
    printf("%s\n", v ? "true" : "false");
}

void tiq_print_f64(double v) {
    printf("%.17g\n", v);
}

// --- String operations -----------------------------------------------------

const char *tiq_str_cat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char *r = (char *)tiq_alloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

const char *tiq_int_str(int64_t n) {
    char *r = (char *)tiq_alloc(21);
    snprintf(r, 21, "%lld", (long long)n);
    return r;
}

const char *tiq_str_sub(const char *s, int64_t start, int64_t end) {
    if (!s) return tiq_str_dup("");
    int64_t len = (int64_t)strlen(s);
    if (start < 0 || end < start || end > len) return tiq_str_dup("");
    size_t n = (size_t)(end - start);
    char *r = (char *)tiq_alloc(n + 1);
    memcpy(r, s + start, n);
    r[n] = '\0';
    return r;
}

int64_t tiq_str_eq(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b) == 0 ? 1 : 0;
}

int64_t tiq_str_len(const char *s) {
    return s ? (int64_t)strlen(s) : 0;
}

// --- CLI arguments ---------------------------------------------------------

int64_t tiq_argc_global = 0;
char **tiq_argv_global = NULL;

void tiq_set_args(int64_t argc, char **argv) {
    tiq_argc_global = argc;
    tiq_argv_global = argv;
}

int64_t tiq_cli_arg_count(void) {
    return tiq_argc_global > 0 ? tiq_argc_global - 1 : 0;
}

const char *tiq_cli_arg(int64_t i) {
    if (i < 0 || i + 1 >= tiq_argc_global) return "";
    return tiq_argv_global[i + 1];
}

// --- Clock -----------------------------------------------------------------

int64_t tiq_clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- Entry point wrapper ---------------------------------------------------
// QBE generates $main as the entry point. We need a C `main` that calls it.
// This is handled by the linker: we provide `main` here that calls
// `tiq_user_main` (the QBE-compiled function).
//
// `main` must `exit(0)`, never `return 0`: the integrated native linkers
// (Mach-O, ELF, PE) point the process entry at this function directly, so
// there is no C runtime wrapper around it to consume the return value.
// A `ret` here would pop the linker-provided argc (1) as the return address
// and crash (issue #7, 2026-08-10).

extern void tiq_user_main(void);

int main(int argc, char **argv) {
    tiq_set_args((int64_t)argc, argv);
    tiq_user_main();
    exit(0);
}
