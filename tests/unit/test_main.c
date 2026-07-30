// Tiq unit test harness (plan item 0.1).
// Single-file C11 runner, no external dependencies. Links against the
// compiler objects (all except main.o). Covers lexer token streams,
// parser AST shapes, type pool interning identity, and environment
// define/lookup/shadowing.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdalign.h>

#include "../../include/arena.h"
#include "../../include/lexer.h"
#include "../../include/parser.h"
#include "../../include/semantic.h"
#include "../../include/type.h"
#include "../../include/emit_c.h"

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s (in %s)\n", __FILE__, __LINE__, #cond, __func__); \
    } \
} while (0)

// ---------------------------------------------------------------- lexer

static void lex_all(const char *source, Token *out, int max, int *count) {
    DiagContext diag;
    diag_init(&diag);
    Lexer lexer;
    lexer_init(&lexer, source, "<unit>", &diag);
    int n = 0;
    for (;;) {
        Token t = lexer_next(&lexer);
        if (n < max) out[n] = t;
        n++;
        if (t.kind == TOK_EOF) break;
    }
    *count = n;
}

static void test_lexer_basic_stream(void) {
    Token toks[16];
    int n = 0;
    lex_all("x = 1", toks, 16, &n);
    ASSERT(n == 4);
    ASSERT(toks[0].kind == TOK_IDENT);
    ASSERT(toks[0].length == 1 && toks[0].start[0] == 'x');
    ASSERT(toks[1].kind == TOK_EQ);
    ASSERT(toks[2].kind == TOK_INT);
    ASSERT(toks[3].kind == TOK_EOF);
}

static void test_lexer_comment_does_not_disturb_stream(void) {
    // Comments are trivia: the main token stream must be identical
    // with or without them.
    Token with_comment[16], without_comment[16];
    int n1 = 0, n2 = 0;
    lex_all("x = 1 // keep me\n", with_comment, 16, &n1);
    lex_all("x = 1\n", without_comment, 16, &n2);
    ASSERT(n1 == n2);
    for (int i = 0; i < n1 && i < 16; i++) {
        ASSERT(with_comment[i].kind == without_comment[i].kind);
    }
}

static void test_lexer_comment_only_line(void) {
    Token toks[8];
    int n = 0;
    lex_all("// only a comment\n", toks, 8, &n);
    ASSERT(n == 2);
    ASSERT(toks[0].kind == TOK_NEWLINE);
    ASSERT(toks[1].kind == TOK_EOF);
}

static void test_lexer_operators_and_lines(void) {
    Token toks[32];
    int n = 0;
    lex_all("a <- 2\nb := a + 1", toks, 32, &n);
    ASSERT(n == 10);
    ASSERT(toks[0].kind == TOK_IDENT);
    ASSERT(toks[1].kind == TOK_LARROW);
    ASSERT(toks[2].kind == TOK_INT);
    ASSERT(toks[3].kind == TOK_NEWLINE);
    ASSERT(toks[4].kind == TOK_IDENT);
    ASSERT(toks[4].line == 2);
    ASSERT(toks[5].kind == TOK_COLON_EQ);
    ASSERT(toks[6].kind == TOK_IDENT);
    ASSERT(toks[7].kind == TOK_PLUS);
    ASSERT(toks[8].kind == TOK_INT);
    ASSERT(toks[9].kind == TOK_EOF);
}

static void test_lexer_string_and_keywords(void) {
    Token toks[16];
    int n = 0;
    lex_all("\"hi\" true while", toks, 16, &n);
    ASSERT(n == 4);
    ASSERT(toks[0].kind == TOK_STRING);
    ASSERT(toks[1].kind == TOK_TRUE);
    ASSERT(toks[2].kind == TOK_WHILE);
    ASSERT(toks[3].kind == TOK_EOF);
}

static void test_lexer_unterminated_string_sets_error(void) {
    DiagContext diag;
    diag_init(&diag);
    Lexer lexer;
    lexer_init(&lexer, "\"oops", "<unit>", &diag);
    Token t = lexer_next(&lexer);
    ASSERT(t.kind == TOK_EOF);
    ASSERT(diag.has_error);
}

// --------------------------------------------------------------- parser

static void test_parser_binding_shape(void) {
    DiagContext diag;
    diag_init(&diag);
    Parser parser;
    parser_init(&parser, "x = 1 + 2\n", "<unit>", &diag);
    int count = 0;
    AstNode **stmts = parser_parse(&parser, &count);
    ASSERT(!diag.has_error);
    ASSERT(count == 1);
    ASSERT(stmts[0]->kind == AST_BINDING);
    ASSERT(!stmts[0]->as.binding.is_mutable);
    ASSERT(stmts[0]->as.binding.expr->kind == AST_BINARY);
    ASSERT(stmts[0]->as.binding.expr->as.binary.op == TOK_PLUS);
    ASSERT(stmts[0]->as.binding.expr->as.binary.left->kind == AST_LITERAL);
    ASSERT(stmts[0]->as.binding.expr->as.binary.right->kind == AST_LITERAL);
    parser_free(&parser);
}

static void test_parser_mutable_binding_shape(void) {
    DiagContext diag;
    diag_init(&diag);
    Parser parser;
    parser_init(&parser, "n <- 5\n", "<unit>", &diag);
    int count = 0;
    AstNode **stmts = parser_parse(&parser, &count);
    ASSERT(!diag.has_error);
    ASSERT(count == 1);
    ASSERT(stmts[0]->kind == AST_BINDING);
    ASSERT(stmts[0]->as.binding.is_mutable);
    parser_free(&parser);
}

static void test_parser_precedence_shape(void) {
    // 1 + 2 * 3 must parse as 1 + (2 * 3).
    DiagContext diag;
    diag_init(&diag);
    Parser parser;
    parser_init(&parser, "y = 1 + 2 * 3\n", "<unit>", &diag);
    int count = 0;
    AstNode **stmts = parser_parse(&parser, &count);
    ASSERT(!diag.has_error);
    ASSERT(count == 1);
    AstNode *expr = stmts[0]->as.binding.expr;
    ASSERT(expr->kind == AST_BINARY);
    ASSERT(expr->as.binary.op == TOK_PLUS);
    ASSERT(expr->as.binary.right->kind == AST_BINARY);
    ASSERT(expr->as.binary.right->as.binary.op == TOK_STAR);
    parser_free(&parser);
}

static void test_parser_invalid_input_reports_error(void) {
    // Fail closed: garbage input must set the diagnostic flag.
    DiagContext diag;
    diag_init(&diag);
    Parser parser;
    parser_init(&parser, "x = @\n", "<unit>", &diag);
    int count = 0;
    parser_parse(&parser, &count);
    ASSERT(diag.has_error);
    parser_free(&parser);
}

// ------------------------------------------------------------ type pool

static void test_type_pool_primitive_interning(void) {
    TypePool pool;
    type_pool_init(&pool);
    SemanticType *a = type_get(&pool, TYPE_INT);
    SemanticType *b = type_get(&pool, TYPE_INT);
    SemanticType *c = type_get(&pool, TYPE_BOOL);
    ASSERT(a != NULL);
    ASSERT(a == b); // interning identity: same request, same pointer
    ASSERT(a != c);
    ASSERT(a->kind == TYPE_INT);
    ASSERT(c->kind == TYPE_BOOL);
    type_pool_free(&pool);
}

static void test_type_pool_func_and_array_interning(void) {
    TypePool pool;
    type_pool_init(&pool);
    SemanticType *f1 = type_get_func(&pool, TYPE_INT, 2);
    SemanticType *f2 = type_get_func(&pool, TYPE_INT, 2);
    SemanticType *f3 = type_get_func(&pool, TYPE_INT, 3);
    ASSERT(f1 == f2);
    ASSERT(f1 != f3);

    SemanticType *elem = type_get(&pool, TYPE_INT);
    SemanticType *a1 = type_get_array(&pool, elem, 4);
    SemanticType *a2 = type_get_array(&pool, elem, 4);
    SemanticType *a3 = type_get_array(&pool, elem, 5);
    ASSERT(a1 == a2);
    ASSERT(a1 != a3);
    ASSERT(a1->element_type == elem);

    SemanticType *s1 = type_get_slice(&pool, elem);
    SemanticType *s2 = type_get_slice(&pool, elem);
    ASSERT(s1 == s2);
    ASSERT(s1 != a1);
    type_pool_free(&pool);
}

// ---------------------------------------------------------- environment

static Token make_name(const char *text) {
    Token t;
    t.kind = TOK_IDENT;
    t.start = text;
    t.length = strlen(text);
    t.line = 1;
    return t;
}

static void test_env_define_and_lookup(void) {
    TypePool pool;
    type_pool_init(&pool);
    Environment env;
    env_init(&env, NULL);

    Token x = make_name("x");
    ASSERT(env_define(&env, x, false, type_get(&pool, TYPE_INT)));
    Symbol *sym = env_lookup(&env, x);
    ASSERT(sym != NULL);
    ASSERT(sym->type->kind == TYPE_INT);
    ASSERT(!sym->is_mutable);

    // Redefinition in the same scope must be rejected.
    ASSERT(!env_define(&env, x, true, type_get(&pool, TYPE_BOOL)));

    // Unknown names must not resolve.
    ASSERT(env_lookup(&env, make_name("missing")) == NULL);

    env_free(&env);
    type_pool_free(&pool);
}

static void test_env_shadowing_and_scope_chain(void) {
    TypePool pool;
    type_pool_init(&pool);
    Environment outer, inner;
    env_init(&outer, NULL);
    env_init(&inner, &outer);

    Token x = make_name("x");
    Token y = make_name("y");
    ASSERT(env_define(&outer, x, false, type_get(&pool, TYPE_INT)));
    ASSERT(env_define(&outer, y, false, type_get(&pool, TYPE_STR)));

    // Inner scope may shadow an outer name; lookup finds the inner one.
    ASSERT(env_define(&inner, x, true, type_get(&pool, TYPE_BOOL)));
    Symbol *shadowed = env_lookup(&inner, x);
    ASSERT(shadowed != NULL);
    ASSERT(shadowed->type->kind == TYPE_BOOL);
    ASSERT(shadowed->is_mutable);

    // Names only in the outer scope resolve through the chain.
    Symbol *through = env_lookup(&inner, y);
    ASSERT(through != NULL);
    ASSERT(through->type->kind == TYPE_STR);

    // The outer scope is unaffected by the shadow.
    Symbol *original = env_lookup(&outer, x);
    ASSERT(original != NULL);
    ASSERT(original->type->kind == TYPE_INT);

    env_free(&inner);
    env_free(&outer);
    type_pool_free(&pool);
}

// Struct types intern nominally (plan 3.2/3.3): identity is the declared
// name, and field metadata is owned by the pool, not fixed-size arrays.
static void test_type_pool_struct_interning(void) {
    TypePool pool;
    type_pool_init(&pool);

    Token fields[2] = { make_name("x"), make_name("y") };
    SemanticType *field_types[2] = { type_get(&pool, TYPE_INT), type_get(&pool, TYPE_STR) };
    SemanticType *p1 = type_get_struct(&pool, make_name("Point"), fields, field_types, 2);
    ASSERT(p1 != NULL);
    ASSERT(p1->kind == TYPE_STRUCT);
    ASSERT(p1->field_count == 2);
    ASSERT(p1->struct_name != NULL && strcmp(p1->struct_name, "Point") == 0);
    ASSERT(strcmp(p1->field_names[0], "x") == 0);
    ASSERT(strcmp(p1->field_names[1], "y") == 0);
    ASSERT(p1->field_types[0]->kind == TYPE_INT);
    ASSERT(p1->field_types[1]->kind == TYPE_STR);

    // Nominal identity: the same name interns to the same instance.
    SemanticType *p2 = type_get_struct(&pool, make_name("Point"), fields, field_types, 2);
    ASSERT(p1 == p2);

    // A different name is a different type even with identical fields.
    SemanticType *q = type_get_struct(&pool, make_name("Quad"), fields, field_types, 2);
    ASSERT(q != p1);

    // A bare TYPE_STRUCT request must not alias a named struct.
    ASSERT(type_get(&pool, TYPE_STRUCT) != p1);

    // Diagnostics display the nominal name.
    char buf[64];
    type_display(p1, buf, sizeof buf);
    ASSERT(strcmp(buf, "Point") == 0);

    type_pool_free(&pool); // pool owns the copied names; ASan verifies
}

// ----------------------------------------------------------------- arena

// Bump arena for AST nodes and aux arrays (plan 4.1): grows by whole
// blocks, releases everything in one arena_free.
static void test_arena_alloc_and_growth(void) {
    Arena arena;
    arena_init(&arena);
    ASSERT(arena.block_count == 0);
    char *a = arena_alloc(&arena, 8);
    ASSERT(a != NULL);
    ASSERT(arena.block_count == 1);
    char *b = arena_alloc(&arena, 8);
    ASSERT(b != NULL);
    ASSERT(b != a);
    // Every allocation must be aligned for any object type.
    ASSERT(((uintptr_t)a % alignof(max_align_t)) == 0);
    ASSERT(((uintptr_t)b % alignof(max_align_t)) == 0);
    memset(a, 0xAB, 8);
    memset(b, 0xCD, 8);
    // An oversized request must grow the arena with a dedicated block.
    char *big = arena_alloc(&arena, 256 * 1024);
    ASSERT(big != NULL);
    ASSERT(arena.block_count == 2);
    big[0] = 1;
    big[256 * 1024 - 1] = 2;
    arena_free(&arena);
    ASSERT(arena.block_count == 0);
}

static void test_arena_realloc_preserves_content(void) {
    Arena arena;
    arena_init(&arena);
    char *p = arena_realloc(&arena, NULL, 0, 4);
    ASSERT(p != NULL);
    memcpy(p, "abcd", 4);
    // The newest allocation extends in place (the parser's doubling
    // realloc pattern must not copy on every growth step).
    char *q = arena_realloc(&arena, p, 4, 64);
    ASSERT(q == p);
    // Growing after an interleaved allocation must copy the old bytes.
    (void)arena_alloc(&arena, 16);
    char *r = arena_realloc(&arena, q, 64, 128);
    ASSERT(r != q);
    ASSERT(memcmp(r, "abcd", 4) == 0);
    arena_free(&arena);
}

static void test_arena_reset_reuses_memory(void) {
    Arena arena;
    arena_init(&arena);
    char *first = arena_alloc(&arena, 32);
    (void)arena_alloc(&arena, 32);
    arena_reset(&arena);
    ASSERT(arena.block_count == 1);
    // After reset the retained block is reused from the start.
    char *again = arena_alloc(&arena, 32);
    ASSERT(again == first);
    arena_free(&arena);
}

// ---------------------------------------------------------------- emit_c

// Run compile_to_c on `source` and capture the generated C (plan 2.1:
// the emitter is re-entrant, so it can be unit-tested in-process).
static char *emit_c_capture(const char *source, int *had_error) {
    static char buf[32768];
    FILE *out = tmpfile();
    if (!out) return NULL;
    DiagContext diag;
    diag_init(&diag);
    compile_to_c("<unit>", source, out, &diag);
    *had_error = diag.has_error;
    long len = ftell(out);
    if (len < 0 || len >= (long)sizeof(buf)) { fclose(out); return NULL; }
    rewind(out);
    size_t r = fread(buf, 1, (size_t)len, out);
    buf[r] = '\0';
    fclose(out);
    return buf;
}

static void test_emit_c_basic_program(void) {
    int had_error = 1;
    char *c = emit_c_capture("x = 1 + 2\nprint(x)\n", &had_error);
    ASSERT(c != NULL);
    ASSERT(!had_error);
    ASSERT(strstr(c, "int main(int argc, char **argv)") != NULL);
    ASSERT(strstr(c, "int64_t x = (1LL + 2LL);") != NULL);
    ASSERT(strstr(c, "printf(\"%lld\\n\", (long long)(x));") != NULL);
}

static void test_emit_c_rejects_semantic_error(void) {
    int had_error = 0;
    char *c = emit_c_capture("x = y\n", &had_error);
    ASSERT(c != NULL);
    ASSERT(had_error);
    // Fail closed: nothing may be emitted for an invalid program.
    ASSERT(c[0] == '\0');
}

static void test_emit_c_is_reentrant(void) {
    // Two consecutive compilations must not leak state (stream gen table
    // used to be a mutable file-static global).
    int err1 = 1, err2 = 1;
    char *first = emit_c_capture("fib = [0, 1, ... a + b]\nprint(fib[5])\n", &err1);
    ASSERT(first != NULL);
    ASSERT(!err1);
    ASSERT(strstr(first, "tiq_gen_fib") != NULL);
    char *second = emit_c_capture("x = 1\nprint(x)\n", &err2);
    ASSERT(second != NULL);
    ASSERT(!err2);
    ASSERT(strstr(second, "tiq_gen_") == NULL);
}

// ----------------------------------------------------------------- main

int main(void) {
    test_lexer_basic_stream();
    test_lexer_comment_does_not_disturb_stream();
    test_lexer_comment_only_line();
    test_lexer_operators_and_lines();
    test_lexer_string_and_keywords();
    test_lexer_unterminated_string_sets_error();

    test_parser_binding_shape();
    test_parser_mutable_binding_shape();
    test_parser_precedence_shape();
    test_parser_invalid_input_reports_error();

    test_type_pool_primitive_interning();
    test_type_pool_func_and_array_interning();

    test_env_define_and_lookup();
    test_env_shadowing_and_scope_chain();

    test_type_pool_struct_interning();

    test_arena_alloc_and_growth();
    test_arena_realloc_preserves_content();
    test_arena_reset_reuses_memory();

    test_emit_c_basic_program();
    test_emit_c_rejects_semantic_error();
    test_emit_c_is_reentrant();

    if (tests_failed > 0) {
        fprintf(stderr, "unit: %d/%d assertions failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("unit: ok (%d assertions)\n", tests_run);
    return 0;
}
