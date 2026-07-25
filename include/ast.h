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
    AST_BRACKET_EXPR,
    AST_ARRAY,
    AST_DEFER,
    AST_ARRAY_FILL,
    AST_FIELD_ACCESS,
    AST_STRUCT_DEF,
    AST_RECORD_LIT,
    AST_MATCH,
    AST_SPAWN,
    AST_CHAN
} AstKind;

typedef struct AstNode AstNode;

typedef struct {
    AstNode *pattern;
    AstNode *body;
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
        } function;

        struct {
            AstNode *domain;
            AstNode **body_stmts;
            int body_count;
            AstNode *body_final;
        } bracket_loop;

        struct {
            AstNode **seeds;
            int seed_count;
            AstNode *gen_expr;
            AstNode *bound;
        } stream_gen;

        struct {
            AstNode *expr;
        } bracket_expr;

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
    } as;
};

#endif
