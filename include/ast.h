#ifndef TIQ_AST_H
#define TIQ_AST_H

#include "lexer.h"
#include <stdbool.h>

typedef enum {
    AST_LITERAL,
    AST_IDENTIFIER,
    AST_BINARY,
    AST_UNARY,
    AST_CONDITIONAL,
    AST_CALL,
    AST_BLOCK,
    AST_BINDING,
    AST_ASSIGN,
    AST_FUNCTION,
    AST_BRACKET_LOOP,
    AST_BREAK,
    AST_SKIP,
    AST_STREAM_GEN,
    AST_ARRAY,
    AST_DEFER,
    AST_ARRAY_FILL,
    AST_FIELD_ACCESS,
    AST_STRUCT_DEF,
    AST_ENUM_DEF,
    AST_RECORD_LIT,
    AST_MATCH,
    AST_SPAWN,
    AST_CHAN,
    AST_IMPORT,
    // M16.1: extern "C" declaration (LANGUAGE_SPEC §7.1). Reuses the
    // `function` union member (params, annotations; body == NULL) and
    // stores the ABI string token in `token`.
    AST_EXTERN
} AstKind;

typedef struct AstNode AstNode;

typedef struct {
    AstNode *pattern;
    AstNode *body;
    bool is_wildcard;  // true for _ => ... wildcard arm
} MatchArm;

struct AstNode {
    AstKind kind;
    Token token;
    void *semantic_type; // Maps to SemanticType* during checking

    union {
        struct {
            TokenKind type;
        } literal;

        struct {
            Token name;
        } identifier;

        struct {
            AstNode *left;
            AstNode *right;
            TokenKind op;
        } binary;

        struct {
            AstNode *right;
            TokenKind op;
            bool is_mut_borrow;
        } unary;

        struct {
            AstNode *cond;
            AstNode *then_branch;
            AstNode *else_branch;
        } conditional;

        struct {
            AstNode *callee;
            AstNode **args;
            int arg_count;
            bool is_bracket_call;
            bool is_slice;
        } call;

        struct {
            AstNode **statements;
            int stmt_count;
            AstNode *final_expr;
            AstNode **deferred;
            int defer_count;
        } block;

        struct {
            Token name;
            bool is_mutable;
            AstNode *expr;
        } binding;

        struct {
            Token name;
            TokenKind op;
            AstNode *index;
            AstNode *expr;
            bool is_definition;
        } assign;

        struct {
            Token name;
            Token *params;
            void **param_types;
            int param_count;
            AstNode *body;
            // M12.4: optional type annotations (kind == TOK_EOF if absent)
            Token *param_type_annots;
            Token return_type_annot;
            // M13.1-P8: vec[T] element type names (kind == TOK_EOF if
            // absent; set only when the annotation name is "vec").
            Token *param_elem_annots;
            Token return_elem_annot;
            // M9.1: borrow spelling per parameter annotation:
            // 0 = by value, 1 = shared borrow (&T), 2 = mutable borrow (&mut T)
            unsigned char *param_ref_kinds;
        } function;

        struct {
            AstNode *domain;
            Token binder;      // optional loop variable name: [j <- 0..10] { ... }
            bool has_binder;   // false = no index variable (M22: no implicit 'i')
            AstNode **body_stmts;
            int body_count;
            AstNode *body_final;
        } bracket_loop;

        // M22: stream generators require explicit binders for the generator
        // expression.  Syntax: [seeds, ... (w1, w2; idx) -> expr]
        // Window binders (w1[, w2]) name the recurrence window values.
        // Optional index binder after ';' names the step counter.
        struct {
            AstNode **seeds;
            int seed_count;
            AstNode *gen_expr;
            AstNode *bound;
            Token *binders;          // window binder name tokens (1 or 2)
            int binder_count;        // number of window binders
            Token index_binder;      // optional index binder (kind==TOK_EOF if absent)
            bool has_index_binder;   // true when index_binder is present
        } stream_gen;

        struct {
            AstNode **elements;
            int element_count;
        } array;

        struct {
            AstNode *value;
            AstNode *length;
        } array_fill;

        struct {
            AstNode *target;
            Token field;
        } field_access;

        struct {
            Token name;
            Token *field_names;
            Token *field_types;
            int field_count;
        } struct_def;

        // M13.1-P2: enum Name { A, B, C } — variants auto-numbered 0..n-1.
        struct {
            Token name;
            Token *variants;
            int variant_count;
        } enum_def;

        struct {
            Token struct_name;
            Token *field_names;
            AstNode **field_values;
            int field_count;
        } record_lit;

        struct {
            AstNode *expr;
            MatchArm *arms;
            int arm_count;
        } match_expr;

        struct {
            AstNode *expr;
        } spawn;

        struct {
            Token elem_type;
        } chan;

        struct {
            AstNode *expr;
        } defer;

        // M13.1-P6: import "path.tiq" — path is the string literal token
        // (quotes included); resolved by the module loader (§17.6).
        struct {
            Token path;
        } import_stmt;
    } as;
};

#endif
