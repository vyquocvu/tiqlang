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
    // Advance past initial newlines and get first token
    do {
        parser->current = lexer_next(&parser->lexer);
    } while (parser->current.kind == TOK_NEWLINE);
}

static AstNode *allocate_node(Parser *parser, AstKind kind) {
    AstNode *node = malloc(sizeof(AstNode));
    if (!node) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memset(node, 0, sizeof(AstNode));
    node->kind = kind;
    node->token = parser->previous;

    if (parser->node_count + 1 > parser->node_capacity) {
        parser->node_capacity = parser->node_capacity < 8 ? 8 : parser->node_capacity * 2;
        parser->nodes = realloc(parser->nodes, sizeof(AstNode *) * parser->node_capacity);
        if (!parser->nodes) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    parser->nodes[parser->node_count++] = node;
    return node;
}

static void advance(Parser *parser) {
    parser->previous = parser->current;

    for (;;) {
        parser->current = lexer_next(&parser->lexer);
        if (parser->current.kind != TOK_NEWLINE) break;
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
    if (check(parser, kind)) {
        advance(parser);
        return;
    }
    error_at_current(parser, code, message);
}

// Forward declarations
static AstNode *expression(Parser *parser);
static AstNode *statement(Parser *parser);
static AstNode *declaration(Parser *parser);
static AstNode *block(Parser *parser) {
    AstNode *node = allocate_node(parser, AST_BLOCK);

    int capacity = 0;
    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        if (node->as.block.stmt_count + 1 > capacity) {
            capacity = capacity < 4 ? 4 : capacity * 2;
            node->as.block.statements = realloc(node->as.block.statements, sizeof(AstNode *) * capacity);
        }

        AstNode *stmt = statement(parser);
        if (parser->diag->fatal_error) break;
        node->as.block.statements[node->as.block.stmt_count++] = stmt;

        // According to grammar: { statement, separator }, [ expression ]
        // We simplified a bit for now to just parsing statements until '}'
    }

    consume(parser, TOK_RBRACE, ERR_UNEXPECTED_TOKEN, "expected '}' after block");

    // For M0/M1 we just treat all as statements in the block.
    // Real implementation will pull the last one as final_expr if no separator.
    return node;
}

static AstNode *primary(Parser *parser) {
    if (match(parser, TOK_FALSE) || match(parser, TOK_TRUE) || match(parser, TOK_INT) || match(parser, TOK_FLOAT) || match(parser, TOK_STRING)) {
        AstNode *node = allocate_node(parser, AST_LITERAL);
        node->as.literal.type = parser->previous.kind;
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

    error_at_current(parser, ERR_EXPECTED_EXPRESSION, "expected expression");
    return NULL;
}

static AstNode *call_or_index(Parser *parser) {
    AstNode *expr = primary(parser);

    while (true) {
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
        } else if (match(parser, TOK_LBRACKET)) {
            // Indexing - not required for M1 but parsing grammar allows it, keeping simple for now
            error_at_current(parser, ERR_UNEXPECTED_TOKEN, "indexing not yet supported");
            break;
        } else {
            break;
        }
    }

    return expr;
}

static AstNode *unary(Parser *parser) {
    if (match(parser, TOK_BANG) || match(parser, TOK_PLUS) || match(parser, TOK_MINUS)) {
        AstNode *node = allocate_node(parser, AST_UNARY);
        node->as.unary.op = parser->previous.kind;
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
        node->as.binary.right = additive(parser);
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

static AstNode *print_statement(Parser *parser) {
    AstNode *node = allocate_node(parser, AST_PRINT);

    // We should make sure it is exactly a string literal for now to match the bootstrap compiler requirements
    if (check(parser, TOK_STRING) || check(parser, TOK_INT) || check(parser, TOK_IDENT)) {
        // We only support STRING but since our expression function allows more, we need to manually error if it isn't STRING
        if (check(parser, TOK_STRING)) {
            node->as.print_stmt.expr = expression(parser);
        } else {
             error_at_current(parser, ERR_EXPECTED_STRING, "bootstrap compiler expects a string literal after '!'");
             node->as.print_stmt.expr = expression(parser);
        }
    } else {
        // Only output if we didn't already have an unterminated string error, which would be reported from the lexer
        if (!parser->diag->has_error) {
            // We want the error to be at the BANG for missing_string.tiq since EOF is line 2
            if (parser->current.kind == TOK_EOF) {
                diag_error(parser->diag, parser->lexer.path, parser->previous.line, ERR_EXPECTED_STRING, "bootstrap compiler expects a string literal after '!'");
            } else {
                error_at_current(parser, ERR_EXPECTED_STRING, "bootstrap compiler expects a string literal after '!'");
            }
        }
        node->as.print_stmt.expr = NULL;
    }
    return node;
}

static AstNode *assign_statement(Parser *parser, Token name) {
    AstNode *node = allocate_node(parser, AST_ASSIGN);
    node->as.assign.name = name;
    node->as.assign.op = parser->previous.kind;
    node->as.assign.expr = expression(parser);
    return node;
}

static AstNode *statement(Parser *parser) {
    if (match(parser, TOK_BANG)) {
        return print_statement(parser);
    }

    if (check(parser, TOK_IDENT)) {
        // Look ahead to check for assignment
        Token name = parser->current;
        Lexer peek_lexer = parser->lexer;
        // peek
        Token next = lexer_next(&peek_lexer);
        while(next.kind == TOK_NEWLINE) {
            next = lexer_next(&peek_lexer);
        }

        if (next.kind == TOK_LARROW || next.kind == TOK_PLUS_EQ || next.kind == TOK_MINUS_EQ ||
            next.kind == TOK_STAR_EQ || next.kind == TOK_SLASH_EQ || next.kind == TOK_PERCENT_EQ) {
            advance(parser); // Consume IDENT
            advance(parser); // Consume OP
            return assign_statement(parser, name);
        }
    }

    return expression(parser);
}

static AstNode *declaration(Parser *parser) {
    if (check(parser, TOK_IDENT)) {
        Token name = parser->current;

        // We need to lookahead a bit. Could be a binding `x = 1`, `x := 1`, a function def `f x y = x + y`, or an assign/expr statement
        // For function def or binding, the structure is IDENT (IDENT)* (=|:=)

        Lexer peek_lexer = parser->lexer;
        Token next = lexer_next(&peek_lexer);
        while (next.kind == TOK_NEWLINE) next = lexer_next(&peek_lexer);

        if (next.kind == TOK_EQ || next.kind == TOK_COLON_EQ) {
            advance(parser); // Consume IDENT
            advance(parser); // Consume EQ / COLON_EQ
            AstNode *node = allocate_node(parser, AST_BINDING);
            node->as.binding.name = name;
            node->as.binding.is_mutable = parser->previous.kind == TOK_COLON_EQ;
            node->as.binding.expr = expression(parser);
            return node;
        } else if (next.kind == TOK_IDENT) {
            // Function definition `f a b = expr`
            advance(parser); // Consume name

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

            consume(parser, TOK_EQ, ERR_UNEXPECTED_TOKEN, "expected '=' after function parameters");
            node->as.function.body = expression(parser);
            return node;
        }
    }

    return statement(parser);
}

AstNode **parser_parse(Parser *parser, int *out_count) {
    AstNode **statements = NULL;
    int count = 0;
    int capacity = 0;

    while (!match(parser, TOK_EOF)) {
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
    // Only free the nodes themselves, any nested pointers to dynamically allocated arrays inside the nodes
    // need to be handled, but for now we haven't allocated any inner arrays like args or params.
    for (int i = 0; i < parser->node_count; i++) {
        if (parser->nodes[i]->kind == AST_CALL) {
            free(parser->nodes[i]->as.call.args);
        } else if (parser->nodes[i]->kind == AST_BLOCK) {
            free(parser->nodes[i]->as.block.statements);
        } else if (parser->nodes[i]->kind == AST_FUNCTION) {
            free(parser->nodes[i]->as.function.params);
        }
        if (parser->nodes[i]->semantic_type) {
            free(parser->nodes[i]->semantic_type);
        }
        free(parser->nodes[i]);
    }
    free(parser->nodes);
}

static const char *type_name(SemanticType *t) {
    if (!t) return "";
    switch (t->kind) {
        case TYPE_INT: return " <TYPE_INT>";
        case TYPE_FLOAT: return " <TYPE_FLOAT>";
        case TYPE_STR: return " <TYPE_STR>";
        case TYPE_BOOL: return " <TYPE_BOOL>";
        default: return " <TYPE_UNKNOWN>";
    }
}

void ast_print(AstNode *node, int indent) {
    if (!node) return;

    for (int i = 0; i < indent; i++) printf("  ");

    const char *t_str = type_name((SemanticType *)node->semantic_type);

    switch (node->kind) {
        case AST_PRINT:
            printf("PRINT%s\n", t_str);
            ast_print(node->as.print_stmt.expr, indent + 1);
            break;
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
            printf("CALL%s\n", t_str);
            ast_print(node->as.call.callee, indent + 1);
            for (int i = 0; i < node->as.call.arg_count; i++) {
                ast_print(node->as.call.args[i], indent + 1);
            }
            break;
        case AST_BLOCK:
            printf("BLOCK%s\n", t_str);
            for (int i = 0; i < node->as.block.stmt_count; i++) {
                ast_print(node->as.block.statements[i], indent + 1);
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
        default:
            printf("UNKNOWN%s\n", t_str);
            break;
    }
}
