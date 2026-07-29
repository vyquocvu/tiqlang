#include "../include/parser.h"
#include "../include/semantic.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void parser_init(Parser *parser, const char *source, const char *path, DiagContext *diag) {
    lexer_init(&parser->lexer, source, path, diag);
    parser->diag = diag;
    arena_init(&parser->arena);
    do {
        parser->current = lexer_next(&parser->lexer);
    } while (parser->current.kind == TOK_NEWLINE);
}

static AstNode *allocate_node(Parser *parser, AstKind kind) {
    AstNode *node = arena_alloc(&parser->arena, sizeof(AstNode));
    memset(node, 0, sizeof(AstNode));
    node->kind = kind;
    node->token = parser->previous;
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
    // Minimal panic recovery: consume the offending token so parse loops
    // always make progress and cannot spin on the same input (fuzz 0.4).
    if (!check(parser, TOK_EOF)) advance(parser);
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
                int new_cap = defer_capacity < 4 ? 4 : defer_capacity * 2;
                node->as.block.deferred = arena_realloc(&parser->arena, node->as.block.deferred,
                                                        sizeof(AstNode *) * defer_capacity,
                                                        sizeof(AstNode *) * new_cap);
                defer_capacity = new_cap;
            }
            node->as.block.deferred[node->as.block.defer_count++] = stmt;
        } else {
            if (node->as.block.stmt_count + 1 > capacity) {
                int new_cap = capacity < 4 ? 4 : capacity * 2;
                node->as.block.statements = arena_realloc(&parser->arena, node->as.block.statements,
                                                          sizeof(AstNode *) * capacity,
                                                          sizeof(AstNode *) * new_cap);
                capacity = new_cap;
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
        if (parser->diag->fatal_error) break;
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
            int new_cap = cap < 4 ? 4 : cap * 2;
            node->as.stream_gen.seeds = arena_realloc(&parser->arena, node->as.stream_gen.seeds,
                                                      sizeof(AstNode *) * cap,
                                                      sizeof(AstNode *) * new_cap);
            cap = new_cap;
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
    // Optional binder names the loop variable: [j <- 0..10] { ... }.
    // '<-' cannot appear inside a header expression, so IDENT '<-' is
    // unambiguous here.
    if (check(parser, TOK_IDENT)) {
        Lexer peek_lexer = parser->lexer;
        Token next = lexer_next(&peek_lexer);
        while (next.kind == TOK_NEWLINE) next = lexer_next(&peek_lexer);
        if (next.kind == TOK_LARROW) {
            advance(parser);
            node->as.bracket_loop.binder = parser->previous;
            node->as.bracket_loop.has_binder = true;
            advance(parser);
        }
    }
    node->as.bracket_loop.domain = expression(parser);
    // Multi-binder headers desugar to nested loops (Cartesian product):
    // [j <- 0..3, k <- 0..j] { body } becomes a k-loop inside the j-loop,
    // so later binders see earlier ones and break/skip stay innermost.
    AstNode *innermost = node;
    while (innermost->as.bracket_loop.has_binder && check(parser, TOK_COMMA)) {
        advance(parser);
        bool clause_ok = false;
        if (check(parser, TOK_IDENT)) {
            Lexer peek_lexer = parser->lexer;
            Token next = lexer_next(&peek_lexer);
            while (next.kind == TOK_NEWLINE) next = lexer_next(&peek_lexer);
            clause_ok = next.kind == TOK_LARROW;
        }
        if (!clause_ok) {
            error_at_current(parser, ERR_UNEXPECTED_TOKEN, "expected loop binder after ','");
            break;
        }
        AstNode *inner = allocate_node(parser, AST_BRACKET_LOOP);
        advance(parser);
        inner->as.bracket_loop.binder = parser->previous;
        inner->as.bracket_loop.has_binder = true;
        advance(parser);
        for (AstNode *outer = node; ; outer = outer->as.bracket_loop.body_stmts[0]) {
            if (outer->as.bracket_loop.binder.length == inner->as.bracket_loop.binder.length &&
                memcmp(outer->as.bracket_loop.binder.start, inner->as.bracket_loop.binder.start,
                       inner->as.bracket_loop.binder.length) == 0) {
                diag_error(parser->diag, parser->lexer.path, inner->token.line,
                           ERR_LOOP_VARIABLE, "duplicate loop binder");
                break;
            }
            if (outer == innermost) break;
        }
        inner->as.bracket_loop.domain = expression(parser);
        innermost->as.bracket_loop.body_stmts = arena_alloc(&parser->arena, sizeof(AstNode *));
        innermost->as.bracket_loop.body_stmts[0] = inner;
        innermost->as.bracket_loop.body_count = 1;
        innermost = inner;
    }
    consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after loop header");
    consume(parser, TOK_LBRACE, ERR_UNEXPECTED_TOKEN, "expected '{' to open loop body");
    if (parser->diag->fatal_error) return node;
    int capacity = 0;
    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        if (innermost->as.bracket_loop.body_count + 1 > capacity) {
            int new_cap = capacity < 4 ? 4 : capacity * 2;
            innermost->as.bracket_loop.body_stmts = arena_realloc(&parser->arena, innermost->as.bracket_loop.body_stmts,
                                                                  sizeof(AstNode *) * capacity,
                                                                  sizeof(AstNode *) * new_cap);
            capacity = new_cap;
        }
        AstNode *stmt = statement(parser);
        if (parser->diag->fatal_error) break;
        if (stmt && stmt->kind == AST_DEFER) {
            diag_error(parser->diag, parser->lexer.path, stmt->token.line,
                       ERR_DEFER_OUTSIDE_BLOCK, "defer is not allowed in bracket loops");
        }
        innermost->as.bracket_loop.body_stmts[innermost->as.bracket_loop.body_count++] = stmt;
        match(parser, TOK_SEMICOLON);
    }
    consume(parser, TOK_RBRACE, ERR_UNEXPECTED_TOKEN, "expected '}' after loop body");
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
            AstNode *pat = NULL;
            bool is_wildcard = false;
            if (match(parser, TOK_UNDERSCORE)) {
                // Wildcard pattern: _ => ...
                is_wildcard = true;
            } else {
                pat = expression(parser);
            }
            if (parser->diag->fatal_error) break;
            consume(parser, TOK_FAT_ARROW, ERR_UNEXPECTED_TOKEN, "expected '=>' in match arm");
            AstNode *arm_body = expression(parser);
            if (parser->diag->fatal_error) break;
            if (node->as.match_expr.arm_count + 1 > cap) {
                int new_cap = cap < 4 ? 4 : cap * 2;
                node->as.match_expr.arms = arena_realloc(&parser->arena, node->as.match_expr.arms,
                                                         sizeof(MatchArm) * cap,
                                                         sizeof(MatchArm) * new_cap);
                cap = new_cap;
            }
            node->as.match_expr.arms[node->as.match_expr.arm_count].pattern = pat;
            node->as.match_expr.arms[node->as.match_expr.arm_count].body = arm_body;
            node->as.match_expr.arms[node->as.match_expr.arm_count].is_wildcard = is_wildcard;
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
        // M12.6: check for record literal: Ident { field: value, ... }
        // But NOT if the next token after '{' is an expression followed by '=>'
        // (that would be a match arm, not a field init)
        if (check(parser, TOK_LBRACE)) {
            // Peek ahead to distinguish record literal from match body
            Lexer peek_lexer = parser->lexer;
            Token peek_tok = lexer_next(&peek_lexer); // skip '{'
            while (peek_tok.kind == TOK_NEWLINE) peek_tok = lexer_next(&peek_lexer);
            // If we see 'ident :' or '}', it's a record literal
            // If we see something else (like a number or '_' followed by '=>'), it's not
            bool is_record_lit = false;
            if (peek_tok.kind == TOK_RBRACE) {
                is_record_lit = true; // empty record literal
            } else if (peek_tok.kind == TOK_IDENT) {
                Token peek_tok2 = lexer_next(&peek_lexer);
                while (peek_tok2.kind == TOK_NEWLINE) peek_tok2 = lexer_next(&peek_lexer);
                if (peek_tok2.kind == TOK_COLON) {
                    is_record_lit = true; // field: value
                }
            }
            if (is_record_lit) {
                AstNode *node = allocate_node(parser, AST_RECORD_LIT);
                node->as.record_lit.struct_name = parser->previous;
                advance(parser); // consume '{'
                int capacity = 0;
                while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
                    if (node->as.record_lit.field_count + 1 > capacity) {
                        int new_cap = capacity < 4 ? 4 : capacity * 2;
                        node->as.record_lit.field_names = arena_realloc(&parser->arena, node->as.record_lit.field_names,
                                                                        sizeof(Token) * capacity,
                                                                        sizeof(Token) * new_cap);
                        node->as.record_lit.field_values = arena_realloc(&parser->arena, node->as.record_lit.field_values,
                                                                         sizeof(AstNode *) * capacity,
                                                                         sizeof(AstNode *) * new_cap);
                        capacity = new_cap;
                    }
                    consume(parser, TOK_IDENT, ERR_UNEXPECTED_TOKEN, "expected field name in record literal");
                    node->as.record_lit.field_names[node->as.record_lit.field_count] = parser->previous;
                    consume(parser, TOK_COLON, ERR_UNEXPECTED_TOKEN, "expected ':' after field name");
                    node->as.record_lit.field_values[node->as.record_lit.field_count] = expression(parser);
                    node->as.record_lit.field_count++;
                    if (check(parser, TOK_COMMA)) advance(parser);
                }
                consume(parser, TOK_RBRACE, ERR_UNEXPECTED_TOKEN, "expected '}' after record literal");
                return node;
            }
        }
        AstNode *node = allocate_node(parser, AST_IDENTIFIER);
        node->as.identifier.name = parser->previous;
        return node;
    }
    if (match(parser, TOK_LBRACKET)) {
        AstNode *node = stream_gen(parser);
        if (node->kind == AST_ARRAY_FILL) return node;
        consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after bracket expression");
        // Convert stream_gen to array: 0 or more seeds, no gen_expr
        if (!node->as.stream_gen.gen_expr && !node->as.stream_gen.bound) {
            // All bracket expressions (empty or with seeds) become arrays
            AstNode **saved = node->as.stream_gen.seeds;
            int count = node->as.stream_gen.seed_count;
            node->kind = AST_ARRAY;
            node->as.array.elements = saved;
            node->as.array.element_count = count;
            return node;
        }
        return node;
    }
    error_at_current(parser, ERR_EXPECTED_EXPRESSION, "expected expression");
    return NULL;
}

static AstNode *call_or_index(Parser *parser) {
    AstNode *expr = primary(parser);
    while (true) {
        if (parser->crossed_newline || parser->diag->fatal_error) break;
        if (match(parser, TOK_LPAREN)) {
            AstNode *node = allocate_node(parser, AST_CALL);
            node->as.call.callee = expr;
            int capacity = 0;
            if (!check(parser, TOK_RPAREN)) {
                do {
                    if (node->as.call.arg_count + 1 > capacity) {
                        int new_cap = capacity < 4 ? 4 : capacity * 2;
                        node->as.call.args = arena_realloc(&parser->arena, node->as.call.args,
                                                           sizeof(AstNode *) * capacity,
                                                           sizeof(AstNode *) * new_cap);
                        capacity = new_cap;
                    }
                    node->as.call.args[node->as.call.arg_count++] = expression(parser);
                } while (!parser->diag->fatal_error && match(parser, TOK_COMMA));
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
                node->as.call.args = arena_alloc(&parser->arena, sizeof(AstNode *));
                node->as.call.args[0] = idx_expr;
            } else if (match(parser, TOK_DOT_DOT)) {
                AstNode *end = check(parser, TOK_RBRACKET) ? NULL : expression(parser);
                node->as.call.is_slice = true;
                node->as.call.arg_count = 2;
                node->as.call.args = arena_alloc(&parser->arena, sizeof(AstNode *) * 2);
                node->as.call.args[0] = NULL;
                node->as.call.args[1] = end;
            } else {
                AstNode *idx_expr = expression(parser);
                if (idx_expr && idx_expr->kind == AST_BINARY && idx_expr->as.binary.op == TOK_DOT_DOT) {
                    node->as.call.is_slice = true;
                    node->as.call.arg_count = 2;
                    node->as.call.args = arena_alloc(&parser->arena, sizeof(AstNode *) * 2);
                    node->as.call.args[0] = idx_expr->as.binary.left;
                    node->as.call.args[1] = idx_expr->as.binary.right;
                } else {
                    node->as.call.arg_count = 1;
                    node->as.call.args = arena_alloc(&parser->arena, sizeof(AstNode *));
                    node->as.call.args[0] = idx_expr;
                }
            }
            consume(parser, TOK_RBRACKET, ERR_UNEXPECTED_TOKEN, "expected ']' after index");
            expr = node;
        } else if (check(parser, TOK_QUESTION)) {
            // M8: Propagation operator (postfix ?) vs ternary conditional (? :).
            // Peek ahead to see if there's a ':' at the same nesting level
            // before a newline/EOF. If yes, it's ternary (leave for conditional()).
            // If no, it's propagation.
            Lexer peek_lexer = parser->lexer;
            Token peek_tok = lexer_next(&peek_lexer); // skip '?'
            int depth = 0;
            bool found_colon = false;
            while (peek_tok.kind != TOK_EOF && peek_tok.kind != TOK_NEWLINE) {
                if (peek_tok.kind == TOK_LPAREN || peek_tok.kind == TOK_LBRACKET || peek_tok.kind == TOK_LBRACE) depth++;
                else if (peek_tok.kind == TOK_RPAREN || peek_tok.kind == TOK_RBRACKET || peek_tok.kind == TOK_RBRACE) {
                    if (depth == 0) break;
                    depth--;
                } else if (peek_tok.kind == TOK_COLON && depth == 0) {
                    found_colon = true;
                    break;
                }
                peek_tok = lexer_next(&peek_lexer);
            }
            if (found_colon) {
                break; // It's ternary conditional, let conditional() handle it
            }
            // It's propagation
            advance(parser); // consume '?'
            AstNode *node = allocate_node(parser, AST_UNARY);
            node->as.unary.op = TOK_QUESTION;
            node->as.unary.right = expr;
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

// M8: fallback operator (??) - precedence between || and ?:
static AstNode *fallback(Parser *parser) {
    AstNode *expr = logical_or(parser);
    while (match(parser, TOK_QUESTION_QUESTION)) {
        AstNode *node = allocate_node(parser, AST_BINARY);
        node->as.binary.op = TOK_QUESTION_QUESTION;
        node->as.binary.left = expr;
        node->as.binary.right = logical_or(parser);
        expr = node;
    }
    return expr;
}

static AstNode *conditional(Parser *parser) {
    AstNode *expr = fallback(parser);
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
    if (!expr) return NULL;

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
    // M12.6: struct definition
    if (match(parser, TOK_STRUCT)) {
        AstNode *node = allocate_node(parser, AST_STRUCT_DEF);
        consume(parser, TOK_IDENT, ERR_UNEXPECTED_TOKEN, "expected struct name after 'struct'");
        node->as.struct_def.name = parser->previous;
        consume(parser, TOK_LBRACE, ERR_UNEXPECTED_TOKEN, "expected '{' after struct name");
        int capacity = 0;
        while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
            if (node->as.struct_def.field_count + 1 > capacity) {
                int new_cap = capacity < 4 ? 4 : capacity * 2;
                node->as.struct_def.field_names = arena_realloc(&parser->arena, node->as.struct_def.field_names,
                                                                sizeof(Token) * capacity,
                                                                sizeof(Token) * new_cap);
                node->as.struct_def.field_types = arena_realloc(&parser->arena, node->as.struct_def.field_types,
                                                                sizeof(Token) * capacity,
                                                                sizeof(Token) * new_cap);
                capacity = new_cap;
            }
            consume(parser, TOK_IDENT, ERR_UNEXPECTED_TOKEN, "expected field name");
            node->as.struct_def.field_names[node->as.struct_def.field_count] = parser->previous;
            consume(parser, TOK_COLON, ERR_UNEXPECTED_TOKEN, "expected ':' after field name");
            consume(parser, TOK_IDENT, ERR_UNEXPECTED_TOKEN, "expected field type");
            node->as.struct_def.field_types[node->as.struct_def.field_count] = parser->previous;
            node->as.struct_def.field_count++;
            if (check(parser, TOK_COMMA)) advance(parser);
        }
        consume(parser, TOK_RBRACE, ERR_UNEXPECTED_TOKEN, "expected '}' after struct fields");
        return node;
    }
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
            node->as.function.return_type_annot.kind = TOK_EOF; // no return type by default
            int capacity = 0;
            while (check(parser, TOK_IDENT)) {
                if (node->as.function.param_count + 1 > capacity) {
                    int new_cap = capacity < 4 ? 4 : capacity * 2;
                    node->as.function.params = arena_realloc(&parser->arena, node->as.function.params,
                                                             sizeof(Token) * capacity,
                                                             sizeof(Token) * new_cap);
                    node->as.function.param_type_annots = arena_realloc(&parser->arena, node->as.function.param_type_annots,
                                                             sizeof(Token) * capacity,
                                                             sizeof(Token) * new_cap);
                    node->as.function.param_ref_kinds = arena_realloc(&parser->arena, node->as.function.param_ref_kinds,
                                                             sizeof(unsigned char) * capacity,
                                                             sizeof(unsigned char) * new_cap);
                    capacity = new_cap;
                }
                node->as.function.params[node->as.function.param_count] = parser->current;
                node->as.function.param_type_annots[node->as.function.param_count].kind = TOK_EOF; // default: no annotation
                node->as.function.param_ref_kinds[node->as.function.param_count] = 0; // default: by value
                advance(parser);
                // M12.4: parse optional :type annotation
                if (check(parser, TOK_COLON)) {
                    advance(parser); // consume ':'
                    // M9.1: optional borrow prefix &type / &mut type
                    if (check(parser, TOK_AMP)) {
                        advance(parser); // consume '&'
                        if (check(parser, TOK_MUT)) {
                            advance(parser); // consume 'mut'
                            node->as.function.param_ref_kinds[node->as.function.param_count] = 2;
                        } else {
                            node->as.function.param_ref_kinds[node->as.function.param_count] = 1;
                        }
                    }
                    if (check(parser, TOK_IDENT)) {
                        node->as.function.param_type_annots[node->as.function.param_count] = parser->current;
                        advance(parser);
                    } else if (check(parser, TOK_LBRACKET)) {
                        // Compound type: [T; N] or []T - store '[' as marker, parse in semantic
                        node->as.function.param_type_annots[node->as.function.param_count] = parser->current;
                        advance(parser); // consume '['
                        // For now, skip until we find the closing ']'
                        int bracket_depth = 1;
                        while (bracket_depth > 0 && !check(parser, TOK_EOF)) {
                            if (check(parser, TOK_LBRACKET)) bracket_depth++;
                            else if (check(parser, TOK_RBRACKET)) bracket_depth--;
                            if (bracket_depth > 0) advance(parser);
                        }
                        if (check(parser, TOK_RBRACKET)) advance(parser); // consume final ']'
                    } else {
                        diag_error(parser->diag, parser->lexer.path, parser->current.line,
                                   ERR_UNEXPECTED_TOKEN, "expected type name after ':'");
                    }
                }
                node->as.function.param_count++;
            }
            // param_types is filled in by semantic analysis; it must live
            // in the same arena as the node it annotates (plan 4.1).
            node->as.function.param_types = arena_alloc(&parser->arena,
                sizeof(void *) * (node->as.function.param_count > 0 ? node->as.function.param_count : 1));
            memset(node->as.function.param_types, 0,
                   sizeof(void *) * (node->as.function.param_count > 0 ? node->as.function.param_count : 1));
            consume(parser, TOK_RARROW, ERR_UNEXPECTED_TOKEN, "expected '->' after function parameters");
            // M12.4: parse optional return type annotation: -> type -> body
            if (check(parser, TOK_IDENT)) {
                // Could be return type or start of body expression
                // Peek ahead: if next is '->', this is a return type
                Lexer peek_lexer2 = parser->lexer;
                Token peek_tok = lexer_next(&peek_lexer2);
                while (peek_tok.kind == TOK_NEWLINE) peek_tok = lexer_next(&peek_lexer2);
                if (peek_tok.kind == TOK_RARROW) {
                    // This is a return type annotation
                    node->as.function.return_type_annot = parser->current;
                    advance(parser); // consume type name
                    consume(parser, TOK_RARROW, ERR_UNEXPECTED_TOKEN, "expected '->' after return type");
                }
            }
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

// The returned statements array (and every node it references) lives in
// the parser's arena: callers must not free it, only call parser_free.
AstNode **parser_parse(Parser *parser, int *out_count) {
    AstNode **statements = NULL;
    int count = 0;
    int capacity = 0;
    while (!match(parser, TOK_EOF)) {
        while (check(parser, TOK_NEWLINE)) advance(parser);
        AstNode *decl = declaration(parser);
        if (parser->diag->fatal_error) break;
        if (count + 1 > capacity) {
            int new_cap = capacity < 8 ? 8 : capacity * 2;
            statements = arena_realloc(&parser->arena, statements,
                                       sizeof(AstNode *) * capacity,
                                       sizeof(AstNode *) * new_cap);
            capacity = new_cap;
        }
        statements[count++] = decl;
    }
    *out_count = count;
    return statements;
}

void parser_free(Parser *parser) {
    // Every AstNode and node-owned aux array lives in the arena: one
    // release replaces the per-node partial-free bookkeeping (plan 4.1).
    // semantic_type instances are owned by the TypePool, not the AST.
    arena_free(&parser->arena);
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
        case TYPE_UNIT: return "TYPE_UNIT";
        case TYPE_I8: return "TYPE_I8";
        case TYPE_I16: return "TYPE_I16";
        case TYPE_I32: return "TYPE_I32";
        case TYPE_U8: return "TYPE_U8";
        case TYPE_U16: return "TYPE_U16";
        case TYPE_U32: return "TYPE_U32";
        case TYPE_U64: return "TYPE_U64";
        case TYPE_F32: return "TYPE_F32";
        case TYPE_NEVER: return "TYPE_NEVER";
        case TYPE_OPTION: return "TYPE_OPTION";
        case TYPE_RESULT: return "TYPE_RESULT";
        case TYPE_STRUCT: return "TYPE_STRUCT";
        case TYPE_REF: return "TYPE_REF";
        case TYPE_REF_MUT: return "TYPE_REF_MUT";
        default: return "TYPE_UNKNOWN";
    }
}

static void dump_type_display(SemanticType *t, char *buf, size_t cap) {
    if (!t || cap == 0) { if (cap) buf[0] = '\0'; return; }
    if (t->kind == TYPE_ARRAY) {
        char elem[96] = "";
        dump_type_display(t->element_type, elem, sizeof elem);
        snprintf(buf, cap, "TYPE_ARRAY[%d]%s%s", t->array_length,
                 elem[0] ? ":" : "", elem);
    } else if (t->kind == TYPE_SLICE) {
        char elem[96] = "";
        dump_type_display(t->element_type, elem, sizeof elem);
        snprintf(buf, cap, "TYPE_SLICE%s%s", elem[0] ? ":" : "", elem);
    } else if (t->kind == TYPE_OPTION) {
        char inner[96] = "";
        dump_type_display(t->inner_type, inner, sizeof inner);
        snprintf(buf, cap, "TYPE_OPTION%s%s", inner[0] ? "<" : "", inner);
        if (inner[0]) { size_t l = strlen(buf); if (l + 1 < cap) { buf[l] = '>'; buf[l+1] = '\0'; } }
    } else if (t->kind == TYPE_RESULT) {
        char inner[64] = "", err[64] = "";
        dump_type_display(t->inner_type, inner, sizeof inner);
        dump_type_display(t->error_type, err, sizeof err);
        snprintf(buf, cap, "TYPE_RESULT<%s,%s>", inner, err);
    } else {
        snprintf(buf, cap, "%s", type_kind_name(t->kind));
    }
}

static void format_type(SemanticType *t, char *buf, size_t cap) {
    if (!t) { buf[0] = '\0'; return; }
    char body[128];
    dump_type_display(t, body, sizeof body);
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
            if (node->as.bracket_loop.has_binder) {
                printf("BRACKET_LOOP %.*s%s\n", (int)node->as.bracket_loop.binder.length,
                       node->as.bracket_loop.binder.start, t_str);
            } else {
                printf("BRACKET_LOOP%s\n", t_str);
            }
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
