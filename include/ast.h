#ifndef TIQ_AST_H
#define TIQ_AST_H

#include "lexer.h"
#include <stdbool.h>

typedef enum {
    AST_PRINT,
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
    AST_ARRAY
} AstKind;

typedef struct AstNode AstNode;

struct AstNode {
    AstKind kind;
    Token token;
    void *semantic_type; // Maps to SemanticType* during checking

    union {
        struct {
            AstNode *expr;
        } print_stmt;

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
        } assign;

        struct {
            Token name;
            Token *params;
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
    } as;
};

#endif
