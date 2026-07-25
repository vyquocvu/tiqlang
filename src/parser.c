#include "../include/parser.h"
#include "../include/semantic.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void parser_init(Parser *parser, const char *source, const char *path, DiagContext *diag) {
    lexer_init(&parser->lexer, source, path, diag);
    parser->diag = diag;
    parser->nodes = NULL;
    parser->node_count = 0;
    parser->node_capacity = 0;
    do {
        parser->current = lexer_next(&parser->lexer);
    } while (parser->current.kind == TOK_NEWLINE);
}

static AstNode *allocate_node(Parser *parser, AstKind kind) {
    AstNode *node = malloc(sizeof(AstNode));
    if (!node) { fprintf(stderr, "out of memory\n"); exit(1); }
    memset(node, 0, sizeof(AstNode));
    node->kind = kind;
    node->token = parser->previous;
    if (parser->node_count + 1 > parser->node_capacity) {
        parser->node_capacity = parser->node_capacity < 8 ? 8 : parser->node_capacity * 2;
        parser->nodes = realloc(parser->nodes, sizeof(AstNode *) * parser->node_capacity);
        if (!parser->nodes) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    parser->nodes[parser->node_count++] = node;
    return node;
}

static void advance(Parser *parser) {
    parser->previous = parser->current;
    parser->crossed_newline = false;
    for (;;) {
        parser->current = lexer_next(&parser->lexer);
        if (parser->current.kind != TOK_NEWLINE) break;
        parser->crossed_newline = true;
    }
}

static bool check(Parser *parser, TokenKind kind) {
    return parser->current.kind == kind;
}

static bool match(Parser *parser, TokenKind kind) {
    if (!check(parser, kind)) return false;
    advance(parser);
    return true;
}

static void error_at_current(Parser *parser, ErrorCode code, const char *message) {
    if (parser->diag->fatal_error) return;
    diag_error(parser->diag, parser->lexer.path, parser->current.line, code, message);
}

static void consume(Parser *parser, TokenKind kind, ErrorCode code, const char *message) {
    if (check(parser, kind)) { advance(parser); return; }
    error_at_current(parser, code, message);
}

static AstNode *expression(Parser *parser);
static AstNode *statement(Parser *parser);
static AstNode *declaration(Parser *parser);
static AstNode *bit_xor(Parser *parser);

static AstNode *block(Parser *parser) {
    AstNode *node = allocate_node(parser, AST_BLOCK);
    int capacity = 0;
    int defer_capacity = 0;
    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        AstNode *stmt = statement(parser);
        if (parser->diag->fatal_error) break;
        if (check(parser, TOK_RBRACE)) {
            node->as.block.final_expr = stmt;
            break;
        }
        if (match(parser, TOK_SEMICOLON) || parser->crossed_newline) {
            if (check(parser, TOK_RBRACE)) {
                node->as.block.final_expr = stmt;
                break;
            }
        }
        if (stmt && stmt->kind == AST_DEFER) {
            if (node->as.block.defer_count + 1 > defer_capacity) {
                defer_capacity = defer_capacity < 4 ? 4 : defer_capacity * 2;
                node->as.block.deferred = realloc(node->as.block.deferred, sizeof(AstNode *) * defer_capacity);
            }
            node->as.block.deferred[node->as.block.defer_count++] = stmt;
        } else {
            if (node->as.block.stmt_count + 1 > capacity) {
                capacity = capacity < 4 ? 4 : capacity * 2;
                node->as.block.statements = realloc(node->as.block.statements, sizeof(AstNode *) * capacity);
            }
            node->as.block.statements[node->as.block.stmt_count++] = stmt;
        }
    }
    consume(parser, TOK_RBRACE, ERR_UNEXPECTED_TOKEN, "expected '}' after block");
    return node;
}

static AstNode *stream_gen(Parser *parser) {
    AstNode *node = allocate_node(parser, AST_STREAM_GEN);
    int cap = 0;
    while (!check(parser, TOK_DOT_DOT_DOT) && !check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
        AstNode *elem = expression(parser);
        if (check(parser, TOK_SEMICOLON) && node->as.stream_gen.seed_count == 0) {
            advance(parser);
            AstNode *len_expr = expression(parser);
            consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after array fill length");
            node->kind = AST_ARRAY_FILL;
            node->as.array_fill.value = elem;
            node->as.array_fill.length = len_expr;
            return node;
        }
        if (node->as.stream_gen.seed_count + 1 > cap) {
            cap = cap < 4 ? 4 : cap * 2;
            node->as.stream_gen.seeds = realloc(node->as.stream_gen.seeds, sizeof(AstNode *) * cap);
        }
        node->as.stream_gen.seeds[node->as.stream_gen.seed_count++] = elem;
        if (!check(parser, TOK_DOT_DOT_DOT) && !check(parser, TOK_RBRACKET)) {
            consume(parser, TOK_COMMA, ERR_UNEXPECTED_TOKEN, "expected ',' in stream generator");
        }
    }
    if (match(parser, TOK_DOT_DOT_DOT)) {
        node->as.stream_gen.gen_expr = expression(parser);
    }
    if (check(parser, TOK_WHILE) || check(parser, TOK_UNTIL)) {
        advance(parser);
        node->as.stream_gen.bound = expression(parser);
    }
    return node;
}

static AstNode *bracket_loop(Parser *parser) {
    AstNode *node = allocate_node(parser, AST_BRACKET_LOOP);
    node->as.bracket_loop.domain = bit_xor(parser);
    if (!match(parser, TOK_PIPE)) {
        error_at_current(parser, ERR_UNEXPECTED_TOKEN, "expected '|' in bracket loop");
        return node;
    }
    int capacity = 0;
    while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
        if (node->as.bracket_loop.body_count + 1 > capacity) {
            capacity = capacity < 4 ? 4 : capacity * 2;
            node->as.bracket_loop.body_stmts = realloc(node->as.bracket_loop.body_stmts, sizeof(AstNode *) * capacity);
        }
        AstNode *stmt = statement(parser);
        if (parser->diag->fatal_error) break;
        if (stmt && stmt->kind == AST_DEFER) {
            diag_error(parser->diag, parser->lexer.path, stmt->token.line,
                       ERR_DEFER_OUTSIDE_BLOCK, "defer is not allowed in bracket loops");
        }
        node->as.bracket_loop.body_stmts[node->as.bracket_loop.body_count++] = stmt;
        if (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
            consume(parser, TOK_COMMA, ERR_UNEXPECTED_TOKEN, "expected ',' or ']' after bracket loop body statement");
        }
    }
    consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after bracket loop body");
    return node;
}

static AstNode *primary(Parser *parser) {
    if (match(parser, TOK_FALSE) || match(parser, TOK_TRUE) || match(parser, TOK_INT) || match(parser, TOK_FLOAT) || match(parser, TOK_STRING)) {
        AstNode *node = allocate_node(parser, AST_LITERAL);
        node->as.literal.type = parser->previous.kind;
        return node;
    }
    if (match(parser, TOK_SPAWN)) {
        AstNode *node = allocate_node(parser, AST_SPAWN);
        node->as.spawn.expr = expression(parser);
        return node;
    }
    if (match(parser, TOK_CHAN)) {
        AstNode *node = allocate_node(parser, AST_CHAN);
        if (check(parser, TOK_IDENT)) {
            advance(parser);
            node->as.chan.elem_type = parser->previous;
        }
        return node;
    }
    if (match(parser, TOK_MATCH)) {
        AstNode *node = allocate_node(parser, AST_MATCH);
        node->as.match_expr.expr = expression(parser);
        consume(parser, TOK_LBRACE, ERR_UNEXPECTED_TOKEN, "expected '{' after match expression");
        int cap = 0;
        while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
            AstNode *pat = expression(parser);
            consume(parser, TOK_FAT_ARROW, ERR_UNEXPECTED_TOKEN, "expected '=>' in match arm");
            AstNode *arm_body = expression(parser);
            if (node->as.match_expr.arm_count + 1 > cap) {
                cap = cap < 4 ? 4 : cap * 2;
                node->as.match_expr.arms = realloc(node->as.match_expr.arms, sizeof(MatchArm) * cap);
            }
            node->as.match_expr.arms[node->as.match_expr.arm_count].pattern = pat;
            node->as.match_expr.arms[node->as.match_expr.arm_count].body = arm_body;
            node->as.match_expr.arm_count++;
            if (check(parser, TOK_COMMA)) advance(parser);
        }
        consume(parser, TOK_RBRACE, ERR_UNEXPECTED_TOKEN, "expected '}' after match arms");
        return node;
    }
    if (match(parser, TOK_LPAREN)) {
        AstNode *expr = expression(parser);
        consume(parser, TOK_RPAREN, ERR_UNEXPECTED_TOKEN, "expected ')' after expression");
        return expr;
    }
    if (match(parser, TOK_LBRACE)) {
        return block(parser);
    }
    if (match(parser, TOK_IDENT)) {
        AstNode *node = allocate_node(parser, AST_IDENTIFIER);
        node->as.identifier.name = parser->previous;
        return node;
    }
    if (match(parser, TOK_LBRACKET)) {
        AstNode *node = stream_gen(parser);
        if (node->kind == AST_ARRAY_FILL) return node;
        if (node->as.stream_gen.seed_count == 0 && !node->as.stream_gen.gen_expr && !node->as.stream_gen.bound) {
            return node;
        }
        consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after bracket expression");
        if (node->as.stream_gen.seed_count > 0 && !node->as.stream_gen.gen_expr) {
            if (node->as.stream_gen.seed_count == 1) {
                AstNode **saved_seeds = node->as.stream_gen.seeds;
                AstNode *inner = saved_seeds[0];
                node->kind = AST_BRACKET_EXPR;
                node->as.bracket_expr.expr = inner;
                free(saved_seeds);
                return node;
            } else {
                AstNode **saved = node->as.stream_gen.seeds;
                int count = node->as.stream_gen.seed_count;
                node->kind = AST_ARRAY;
                node->as.array.elements = saved;
                node->as.array.element_count = count;
                return node;
            }
        }
        return node;
    }
    error_at_current(parser, ERR_EXPECTED_EXPRESSION, "expected expression");
    return NULL;
}

static AstNode *call_or_index(Parser *parser) {
    AstNode *expr = primary(parser);
    while (true) {
        if (parser->crossed_newline) break;
        if (match(parser, TOK_LPAREN)) {
            AstNode *node = allocate_node(parser, AST_CALL);
            node->as.call.callee = expr;
            int capacity = 0;
            if (!check(parser, TOK_RPAREN)) {
                do {
                    if (node->as.call.arg_count + 1 > capacity) {
                        capacity = capacity < 4 ? 4 : capacity * 2;
                        node->as.call.args = realloc(node->as.call.args, sizeof(AstNode *) * capacity);
                    }
                    node->as.call.args[node->as.call.arg_count++] = expression(parser);
                } while (match(parser, TOK_COMMA));
            }
            consume(parser, TOK_RPAREN, ERR_UNEXPECTED_TOKEN, "expected ')' after arguments");
            expr = node;
        } else if (match(parser, TOK_DOT)) {
            AstNode *node = allocate_node(parser, AST_FIELD_ACCESS);
            node->as.field_access.target = expr;
            consume(parser, TOK_IDENT, ERR_UNEXPECTED_TOKEN, "expected field identifier after '.'");
            node->as.field_access.field = parser->previous;
            expr = node;
        } else if (match(parser, TOK_LBRACKET)) {
            AstNode *node = allocate_node(parser, AST_CALL);
            node->as.call.callee = expr;
            node->as.call.is_bracket_call = true;
            node->as.call.is_slice = false;
            if (check(parser, TOK_WHILE) || check(parser, TOK_UNTIL)) {
                advance(parser);
                AstNode *idx_expr = expression(parser);
                node->as.call.arg_count = 1;
                node->as.call.args = malloc(sizeof(AstNode *));
                node->as.call.args[0] = idx_expr;
            } else if (match(parser, TOK_DOT_DOT)) {
                AstNode *end = check(parser, TOK_RBRACKET) ? NULL : expression(parser);
                node->as.call.is_slice = true;
                node->as.call.arg_count = 2;
                node->as.call.args = malloc(sizeof(AstNode *) * 2);
                node->as.call.args[0] = NULL;
                node->as.call.args[1] = end;
            } else {
                AstNode *idx_expr = expression(parser);
                if (idx_expr && idx_expr->kind == AST_BINARY && idx_expr->as.binary.op == TOK_DOT_DOT) {
                    node->as.call.is_slice = true;
                    node->as.call.arg_count = 2;
                    node->as.call.args = malloc(sizeof(AstNode *) * 2);
                    node->as.call.args[0] = idx_expr->as.binary.left;
                    node->as.call.args[1] = idx_expr->as.binary.right;
                } else {
                    node->as.call.arg_count = 1;
                    node->as.call.args = malloc(sizeof(AstNode *));
                    node->as.call.args[0] = idx_expr;
                }
            }
            consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after index");
            expr = node;
        } else {
            break;
        }
    }
    return expr;
}

static AstNode *unary(Parser *parser) {
    if (match(parser, TOK_BANG) || match(parser, TOK_PLUS) || match(parser, TOK_MINUS) || match(parser, TOK_MOVE)) {
        AstNode *node = allocate_node(parser, AST_UNARY);
        node->as.unary.op = parser->previous.kind;
        node->as.unary.right = unary(parser);
        return node;
    }
    if (match(parser, TOK_AMP)) {
        AstNode *node = allocate_node(parser, AST_UNARY);
        node->as.unary.op = TOK_AMP;
        if (match(parser, TOK_MUT)) {
            node->as.unary.is_mut_borrow = true;
        }
        node->as.unary.right = unary(parser);
        return node;
    }
    return call_or_index(parser);
}

static AstNode *multiplicative(Parser *parser) {
    AstNode *expr = unary(parser);
    while (match(parser, TOK_STAR) || match(parser, TOK_SLASH) || match(parser, TOK_PERCENT)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = unary(parser);
        expr = node;
    }
    return expr;
}

static AstNode *additive(Parser *parser) {
    AstNode *expr = multiplicative(parser);
    while (match(parser, TOK_PLUS) || match(parser, TOK_MINUS)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = multiplicative(parser);
        expr = node;
    }
    return expr;
}

static AstNode *range(Parser *parser) {
    AstNode *expr = additive(parser);
    if (match(parser, TOK_DOT_DOT)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = TOK_DOT_DOT;
        node->as.binary.left = expr;
        node->as.binary.right = check(parser, TOK_RBRACKET) ? NULL : additive(parser);
        return node;
    }
    return expr;
}

static AstNode *shift(Parser *parser) {
    AstNode *expr = range(parser);
    while (match(parser, TOK_LSHIFT) || match(parser, TOK_RSHIFT)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = range(parser);
        expr = node;
    }
    return expr;
}

static AstNode *comparison(Parser *parser) {
    AstNode *expr = shift(parser);
    while (match(parser, TOK_LT) || match(parser, TOK_LTE) || match(parser, TOK_GT) || match(parser, TOK_GTE)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = shift(parser);
        expr = node;
    }
    return expr;
}

static AstNode *equality(Parser *parser) {
    AstNode *expr = comparison(parser);
    while (match(parser, TOK_EQ_EQ) || match(parser, TOK_BANG_EQ)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = comparison(parser);
        expr = node;
    }
    return expr;
}

static AstNode *bit_and(Parser *parser) {
    AstNode *expr = equality(parser);
    while (match(parser, TOK_AMP)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = equality(parser);
        expr = node;
    }
    return expr;
}

static AstNode *bit_xor(Parser *parser) {
    AstNode *expr = bit_and(parser);
    while (match(parser, TOK_CARET)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = bit_and(parser);
        expr = node;
    }
    return expr;
}

static AstNode *bit_or(Parser *parser) {
    AstNode *expr = bit_xor(parser);
    while (match(parser, TOK_PIPE)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = bit_xor(parser);
        expr = node;
    }
    return expr;
}

static AstNode *logical_and(Parser *parser) {
    AstNode *expr = bit_or(parser);
    while (match(parser, TOK_AND_AND)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = bit_or(parser);
        expr = node;
    }
    return expr;
}

static AstNode *logical_or(Parser *parser) {
    AstNode *expr = logical_and(parser);
    while (match(parser, TOK_OR_OR)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = parser->previous.kind;
        node->as.binary.left = expr;
        node->as.binary.right = logical_and(parser);
        expr = node;
    }
    return expr;
}

static AstNode *conditional(Parser *parser) {
    AstNode *expr = logical_or(parser);
    if (match(parser, TOK_QUESTION)) {
        AstNode *node = allocate_node(parser, AST_CONDITIONAL);
        node->as.conditional.cond = expr;
        node->as.conditional.then_branch = expression(parser);
        consume(parser, TOK_COLON, ERR_UNEXPECTED_TOKEN, "expected ':' in conditional expression");
        node->as.conditional.else_branch = expression(parser);
        return node;
    }
    return expr;
}

static AstNode *expression(Parser *parser) {
    return conditional(parser);
}

static AstNode *assign_statement(Parser *parser, Token name) {
    AstNode *node = allocate_node(parser, AST_ASSIGN);
    node->as.assign.name = name;
    node->as.assign.op = parser->previous.kind;
    node->as.assign.expr = expression(parser);
    return node;
}

static AstNode *control_statement(Parser *parser) {
    if (match(parser, TOK_BREAK)) {
        return allocate_node(parser, AST_BREAK);
    }
    if (match(parser, TOK_SKIP)) {
        return allocate_node(parser, AST_SKIP);
    }
    return NULL;
}

static AstNode *statement(Parser *parser) {
    AstNode *ctrl = control_statement(parser);
    if (ctrl) return ctrl;

    if (match(parser, TOK_DEFER)) {
        AstNode *node = allocate_node(parser, AST_DEFER);
        node->as.defer.expr = statement(parser);
        return node;
    }

    if (match(parser, TOK_LBRACKET)) {
        return bracket_loop(parser);
    }

    if (check(parser, TOK_IDENT)) {
        Token name = parser->current;
        Lexer peek_lexer = parser->lexer;
        Token next = lexer_next(&peek_lexer);
        while (next.kind == TOK_NEWLINE) next = lexer_next(&peek_lexer);
        if (next.kind == TOK_EQ || next.kind == TOK_LARROW || next.kind == TOK_PLUS_EQ ||
            next.kind == TOK_MINUS_EQ || next.kind == TOK_STAR_EQ || next.kind == TOK_SLASH_EQ ||
            next.kind == TOK_PERCENT_EQ) {
            advance(parser);
            advance(parser);
            return assign_statement(parser, name);
        }
    }

    AstNode *expr = expression(parser);

    if (expr->kind == AST_CALL && expr->as.call.is_bracket_call &&
        (check(parser, TOK_LARROW) || check(parser, TOK_PLUS_EQ) ||
         check(parser, TOK_MINUS_EQ) || check(parser, TOK_STAR_EQ) ||
         check(parser, TOK_SLASH_EQ) || check(parser, TOK_PERCENT_EQ))) {
        AstNode *assign = allocate_node(parser, AST_ASSIGN);
        assign->as.assign.name = expr->as.call.callee->as.identifier.name;
        assign->as.assign.op = parser->current.kind;
        assign->as.assign.index = expr->as.call.arg_count > 0 ? expr->as.call.args[0] : NULL;
        advance(parser);
        assign->as.assign.expr = expression(parser);
        return assign;
    }

    return expr;
}

static AstNode *declaration(Parser *parser) {
    if (check(parser, TOK_IDENT)) {
        Token name = parser->current;
        Lexer peek_lexer = parser->lexer;
        Token next = lexer_next(&peek_lexer);
        while (next.kind == TOK_NEWLINE) next = lexer_next(&peek_lexer);
        if (next.kind == TOK_EQ || next.kind == TOK_LARROW) {
            advance(parser);
            advance(parser);
            AstNode *node = allocate_node(parser, AST_BINDING);
            node->as.binding.name = name;
            node->as.binding.is_mutable = parser->previous.kind == TOK_LARROW;
            node->as.binding.expr = expression(parser);
            return node;
        } else if (next.kind == TOK_IDENT) {
            advance(parser);
            AstNode *node = allocate_node(parser, AST_FUNCTION);
            node->as.function.name = name;
            int capacity = 0;
            while (check(parser, TOK_IDENT)) {
                if (node->as.function.param_count + 1 > capacity) {
                    capacity = capacity < 4 ? 4 : capacity * 2;
                    node->as.function.params = realloc(node->as.function.params, sizeof(Token) * capacity);
                }
                node->as.function.params[node->as.function.param_count++] = parser->current;
                advance(parser);
            }
            consume(parser, TOK_RARROW, ERR_UNEXPECTED_TOKEN, "expected '->' after function parameters");
            node->as.function.body = expression(parser);
            return node;
        }
    }
    AstNode *stmt = statement(parser);
    if (stmt && stmt->kind == AST_DEFER) {
        diag_error(parser->diag, parser->lexer.path, stmt->token.line,
                   ERR_DEFER_OUTSIDE_BLOCK, "defer is not allowed outside a block");
    }
    return stmt;
}

AstNode **parser_parse(Parser *parser, int *out_count) {
    AstNode **statements = NULL;
    int count = 0;
    int capacity = 0;
    while (!match(parser, TOK_EOF)) {
        while (check(parser, TOK_NEWLINE)) advance(parser);
        AstNode *decl = declaration(parser);
        if (parser->diag->fatal_error) break;
        if (count + 1 > capacity) {
            capacity = capacity < 8 ? 8 : capacity * 2;
            statements = realloc(statements, sizeof(AstNode *) * capacity);
        }
        statements[count++] = decl;
    }
    *out_count = count;
    return statements;
}

void parser_free(Parser *parser) {
    for (int i = 0; i < parser->node_count; i++) {
        if (parser->nodes[i]->kind == AST_CALL) {
            free(parser->nodes[i]->as.call.args);
        } else if (parser->nodes[i]->kind == AST_BLOCK) {
            free(parser->nodes[i]->as.block.statements);
            free(parser->nodes[i]->as.block.deferred);
        } else if (parser->nodes[i]->kind == AST_FUNCTION) {
            free(parser->nodes[i]->as.function.params);
            free(parser->nodes[i]->as.function.param_types);
        } else if (parser->nodes[i]->kind == AST_BRACKET_LOOP) {
            free(parser->nodes[i]->as.bracket_loop.body_stmts);
        } else if (parser->nodes[i]->kind == AST_STREAM_GEN) {
            free(parser->nodes[i]->as.stream_gen.seeds);
        } else if (parser->nodes[i]->kind == AST_ARRAY) {
            free(parser->nodes[i]->as.array.elements);
        }
        // semantic_type instances are owned by the TypePool, not the AST
        free(parser->nodes[i]);
    }
    free(parser->nodes);
}

static const char *type_kind_name(PrimitiveType kind) {
    switch (kind) {
        case TYPE_INT: return "TYPE_INT";
        case TYPE_FLOAT: return "TYPE_FLOAT";
        case TYPE_STR: return "TYPE_STR";
        case TYPE_BOOL: return "TYPE_BOOL";
        case TYPE_ARRAY: return "TYPE_ARRAY";
        case TYPE_SLICE: return "TYPE_SLICE";
        case TYPE_STR_VIEW: return "TYPE_STR_VIEW";
        case TYPE_STREAM: return "TYPE_STREAM";
        default: return "TYPE_UNKNOWN";
    }
}

static void type_display(SemanticType *t, char *buf, size_t cap) {
    if (!t || cap == 0) { if (cap) buf[0] = '\0'; return; }
    if (t->kind == TYPE_ARRAY) {
        char elem[96] = "";
        type_display(t->element_type, elem, sizeof elem);
        snprintf(buf, cap, "TYPE_ARRAY[%d]%s%s", t->array_length,
                 elem[0] ? ":" : "", elem);
    } else if (t->kind == TYPE_SLICE) {
        char elem[96] = "";
        type_display(t->element_type, elem, sizeof elem);
        snprintf(buf, cap, "TYPE_SLICE%s%s", elem[0] ? ":" : "", elem);
    } else {
        snprintf(buf, cap, "%s", type_kind_name(t->kind));
    }
}

static void format_type(SemanticType *t, char *buf, size_t cap) {
    if (!t) { buf[0] = '\0'; return; }
    char body[128];
    type_display(t, body, sizeof body);
    snprintf(buf, cap, " <%s>", body);
}

void ast_print(AstNode *node, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) printf("  ");
    char t_str[160];
    format_type((SemanticType *)node->semantic_type, t_str, sizeof t_str);
    switch (node->kind) {
        case AST_LITERAL:
            if (node->as.literal.type == TOK_STRING) {
                printf("STRING %.*s%s\n", (int)node->token.length, node->token.start, t_str);
            } else if (node->as.literal.type == TOK_INT) {
                printf("INT %.*s%s\n", (int)node->token.length, node->token.start, t_str);
            } else {
                printf("LITERAL%s\n", t_str);
            }
            break;
        case AST_IDENTIFIER:
            printf("IDENT %.*s%s\n", (int)node->as.identifier.name.length, node->as.identifier.name.start, t_str);
            break;
        case AST_BINARY:
            printf("BINARY %s%s\n", token_kind_name(node->as.binary.op), t_str);
            ast_print(node->as.binary.left, indent + 1);
            ast_print(node->as.binary.right, indent + 1);
            break;
        case AST_UNARY:
            printf("UNARY %s%s\n", token_kind_name(node->as.unary.op), t_str);
            ast_print(node->as.unary.right, indent + 1);
            break;
        case AST_CONDITIONAL:
            printf("CONDITIONAL%s\n", t_str);
            ast_print(node->as.conditional.cond, indent + 1);
            ast_print(node->as.conditional.then_branch, indent + 1);
            ast_print(node->as.conditional.else_branch, indent + 1);
            break;
        case AST_CALL:
            if (node->as.call.is_slice) {
                printf("SLICE%s\n", t_str);
            } else {
                printf("CALL%s\n", t_str);
            }
            ast_print(node->as.call.callee, indent + 1);
            for (int i = 0; i < node->as.call.arg_count; i++) {
                if (node->as.call.args[i]) {
                    ast_print(node->as.call.args[i], indent + 1);
                } else {
                    for (int k = 0; k < indent + 1; k++) printf("  ");
                    printf("OMITTED\n");
                }
            }
            break;
        case AST_BLOCK:
            printf("BLOCK%s\n", t_str);
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                ast_print(node->as.block.statements[i], indent + 1);
            }
            for (int i = 0; i < node->as.block.defer_count; i++) {
                ast_print(node->as.block.deferred[i], indent + 1);
            }
            if (node->as.block.final_expr) {
                ast_print(node->as.block.final_expr, indent + 1);
            }
            break;
        case AST_BINDING:
            if (node->as.binding.is_mutable) {
                printf("MUT_BINDING %.*s%s\n", (int)node->as.binding.name.length, node->as.binding.name.start, t_str);
            } else {
                printf("BINDING %.*s%s\n", (int)node->as.binding.name.length, node->as.binding.name.start, t_str);
            }
            ast_print(node->as.binding.expr, indent + 1);
            break;
        case AST_ASSIGN:
            printf("ASSIGN %.*s %s%s\n", (int)node->as.assign.name.length, node->as.assign.name.start, token_kind_name(node->as.assign.op), t_str);
            if (node->as.assign.index) {
                ast_print(node->as.assign.index, indent + 1);
            }
            ast_print(node->as.assign.expr, indent + 1);
            break;
        case AST_FUNCTION:
            printf("FUNCTION %.*s%s\n", (int)node->as.function.name.length, node->as.function.name.start, t_str);
            for (int i = 0; i < node->as.function.param_count; i++) {
                for (int j = 0; j < indent + 1; j++) printf("  ");
                printf("PARAM %.*s\n", (int)node->as.function.params[i].length, node->as.function.params[i].start);
            }
            ast_print(node->as.function.body, indent + 1);
            break;
        case AST_BRACKET_LOOP:
            printf("BRACKET_LOOP%s\n", t_str);
            ast_print(node->as.bracket_loop.domain, indent + 1);
            for (int i = 0; i < node->as.bracket_loop.body_count; i++) {
                ast_print(node->as.bracket_loop.body_stmts[i], indent + 1);
            }
            if (node->as.bracket_loop.body_final) {
                ast_print(node->as.bracket_loop.body_final, indent + 1);
            }
            break;
        case AST_BREAK:
            printf("BREAK%s\n", t_str);
            break;
        case AST_SKIP:
            printf("SKIP%s\n", t_str);
            break;
        case AST_STREAM_GEN:
            printf("STREAM_GEN%s\n", t_str);
            for (int i = 0; i < node->as.stream_gen.seed_count; i++) {
                ast_print(node->as.stream_gen.seeds[i], indent + 1);
            }
            if (node->as.stream_gen.gen_expr) {
                ast_print(node->as.stream_gen.gen_expr, indent + 1);
            }
            if (node->as.stream_gen.bound) {
                ast_print(node->as.stream_gen.bound, indent + 1);
            }
            break;
        case AST_BRACKET_EXPR:
            printf("BRACKET_EXPR%s\n", t_str);
            ast_print(node->as.bracket_expr.expr, indent + 1);
            break;
        case AST_ARRAY:
            printf("ARRAY%s\n", t_str);
            for (int i = 0; i < node->as.array.element_count; i++)
                ast_print(node->as.array.elements[i], indent + 1);
            break;
        case AST_DEFER:
            printf("DEFER%s\n", t_str);
            ast_print(node->as.defer.expr, indent + 1);
            break;
        default:
            printf("UNKNOWN%s\n", t_str);
            break;
    }
}
