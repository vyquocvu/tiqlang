#include "../include/lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

void lexer_init(Lexer *lexer, const char *source, const char *path, DiagContext *diag) {
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->path = path;
    lexer->diag = diag;
    lexer->pending_comment = NULL;
    lexer->pending_comment_length = 0;
}

static bool is_at_end(Lexer *lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer *lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer *lexer) {
    return *lexer->current;
}

static char peek_next(Lexer *lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static bool match(Lexer *lexer, char expected) {
    if (is_at_end(lexer)) return false;
    if (*lexer->current != expected) return false;
    lexer->current++;
    return true;
}

static Token make_token(Lexer *lexer, TokenKind kind, const char *start) {
    Token token;
    token.kind = kind;
    token.start = start;
    token.length = (size_t)(lexer->current - start);
    token.line = lexer->line;
    token.comment_start = lexer->pending_comment;
    token.comment_length = lexer->pending_comment_length;
    lexer->pending_comment = NULL;
    lexer->pending_comment_length = 0;
    return token;
}

static void skip_whitespace(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(lexer);
                break;
            case '/':
                if (peek_next(lexer) == '/') {
                    const char *comment_start = lexer->current;
                    while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                        advance(lexer);
                    }
                    // Keep the comment as trivia for the next token.
                    lexer->pending_comment = comment_start;
                    lexer->pending_comment_length = (size_t)(lexer->current - comment_start);
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static Token string(Lexer *lexer, const char *start) {
    while (peek(lexer) != '"' && !is_at_end(lexer)) {
        if (peek(lexer) == '\n') {
            diag_error(lexer->diag, lexer->path, lexer->line, ERR_NEWLINE_IN_STRING, "newline in string literal");
            lexer->line++;
        }
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer);
            // LANGUAGE_SPEC §4: only \\ \" \n \r \t \0 are valid escapes;
            // anything else fails closed at lex time.
            char e = peek(lexer);
            if (e != '\\' && e != '"' && e != 'n' && e != 'r' && e != 't' && e != '0') {
                char msg[64];
                snprintf(msg, sizeof(msg), "unsupported escape sequence '\\%c'", e);
                diag_error(lexer->diag, lexer->path, lexer->line, ERR_UNEXPECTED_CHAR, msg);
            }
        }
        advance(lexer);
    }

    if (is_at_end(lexer)) {
        diag_error(lexer->diag, lexer->path, lexer->line, ERR_UNTERMINATED_STRING, "unterminated string literal");
        return make_token(lexer, TOK_EOF, start);
    }

    advance(lexer);
    return make_token(lexer, TOK_STRING, start);
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
            c == '_';
}

static Token number(Lexer *lexer, const char *start) {
    while (is_digit(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        advance(lexer); // Consume '.'
        while (is_digit(peek(lexer)) || peek(lexer) == '_') {
            advance(lexer);
        }
        return make_token(lexer, TOK_FLOAT, start);
    }

    return make_token(lexer, TOK_INT, start);
}

static TokenKind check_keyword(Lexer *lexer, const char *start, size_t start_offset, size_t length, const char *rest, TokenKind kind) {
    if ((size_t)(lexer->current - start) == start_offset + length &&
        memcmp(start + start_offset, rest, length) == 0) {
        return kind;
    }
    return TOK_IDENT;
}

static TokenKind identifier_type(Lexer *lexer, const char *start) {
    switch (start[0]) {
        case 'b': return check_keyword(lexer, start, 1, 4, "reak", TOK_BREAK);
        case 'c': return check_keyword(lexer, start, 1, 3, "han", TOK_CHAN);
        case 'd': return check_keyword(lexer, start, 1, 4, "efer", TOK_DEFER);
        case 'e':
            if (lexer->current - start > 1) {
                switch (start[1]) {
                    case 'n': return check_keyword(lexer, start, 2, 2, "um", TOK_ENUM);
                    // M16.1: 'extern' is a reserved word (LANGUAGE_SPEC §4, §7.1).
                    case 'x': return check_keyword(lexer, start, 2, 4, "tern", TOK_EXTERN);
                }
            }
            break;
        // M13.1-P6: 'import' is a reserved word (LANGUAGE_SPEC §4, §17.6).
        case 'i': return check_keyword(lexer, start, 1, 5, "mport", TOK_IMPORT);
        case 'f':
            if (lexer->current - start > 1) {
                switch (start[1]) {
                    case 'a': return check_keyword(lexer, start, 2, 3, "lse", TOK_FALSE);
                }
            }
            break;
        case 'm':
            if (lexer->current - start > 1) {
                switch (start[1]) {
                    case 'a': return check_keyword(lexer, start, 2, 3, "tch", TOK_MATCH);
                    case 'o': return check_keyword(lexer, start, 2, 2, "ve", TOK_MOVE);
                    case 'u': return check_keyword(lexer, start, 2, 1, "t", TOK_MUT);
                }
            }
            break;
        // M22.1: 'none' is a real reserved literal/token (issue #5 Finding 3).
        case 'n': return check_keyword(lexer, start, 1, 3, "one", TOK_NONE);
        case 's':
            if (lexer->current - start > 1) {
                switch (start[1]) {
                    case 'k': return check_keyword(lexer, start, 2, 2, "ip", TOK_SKIP);
                    case 'p': return check_keyword(lexer, start, 2, 3, "awn", TOK_SPAWN);
                    case 't': return check_keyword(lexer, start, 2, 4, "ruct", TOK_STRUCT);
                }
            }
            break;
        case 't': return check_keyword(lexer, start, 1, 3, "rue", TOK_TRUE);
        case 'u': return check_keyword(lexer, start, 1, 4, "ntil", TOK_UNTIL);
        case 'w': return check_keyword(lexer, start, 1, 4, "hile", TOK_WHILE);
        case '_': return (lexer->current - start == 1) ? TOK_UNDERSCORE : TOK_IDENT;
    }
    return TOK_IDENT;
}

static Token identifier(Lexer *lexer, const char *start) {
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
        advance(lexer);
    }
    return make_token(lexer, identifier_type(lexer, start), start);
}

Token lexer_next(Lexer *lexer) {
    skip_whitespace(lexer);
    const char *start = lexer->current;

    if (is_at_end(lexer)) return make_token(lexer, TOK_EOF, start);

    char c = advance(lexer);

    if (c == '\n') {
        Token token = make_token(lexer, TOK_NEWLINE, start);
        lexer->line++;
        return token;
    }

    if (is_alpha(c) || c == '_') return identifier(lexer, start);
    if (is_digit(c)) return number(lexer, start);

    switch (c) {
        case '{': return make_token(lexer, TOK_LBRACE, start);
        case '}': return make_token(lexer, TOK_RBRACE, start);
        case '(': return make_token(lexer, TOK_LPAREN, start);
        case ')': return make_token(lexer, TOK_RPAREN, start);
        case '[': return make_token(lexer, TOK_LBRACKET, start);
        case ']': return make_token(lexer, TOK_RBRACKET, start);
        case ',': return make_token(lexer, TOK_COMMA, start);
        case ';': return make_token(lexer, TOK_SEMICOLON, start);
        case '?':
            if (match(lexer, '?')) return make_token(lexer, TOK_QUESTION_QUESTION, start);
            return make_token(lexer, TOK_QUESTION, start);
        case '^': return make_token(lexer, TOK_CARET, start);
        case '.':
            if (match(lexer, '.')) {
                if (match(lexer, '.')) return make_token(lexer, TOK_DOT_DOT_DOT, start);
                return make_token(lexer, TOK_DOT_DOT, start);
            }
            return make_token(lexer, TOK_DOT, start);
        case '+':
            if (match(lexer, '=')) return make_token(lexer, TOK_PLUS_EQ, start);
            return make_token(lexer, TOK_PLUS, start);
        case '-':
            if (match(lexer, '=')) return make_token(lexer, TOK_MINUS_EQ, start);
            if (match(lexer, '>')) return make_token(lexer, TOK_RARROW, start);
            return make_token(lexer, TOK_MINUS, start);
        case '*':
            if (match(lexer, '=')) return make_token(lexer, TOK_STAR_EQ, start);
            return make_token(lexer, TOK_STAR, start);
        case '/':
            if (match(lexer, '=')) return make_token(lexer, TOK_SLASH_EQ, start);
            return make_token(lexer, TOK_SLASH, start);
        case '%':
            if (match(lexer, '=')) return make_token(lexer, TOK_PERCENT_EQ, start);
            return make_token(lexer, TOK_PERCENT, start);
        case '=':
            if (match(lexer, '=')) return make_token(lexer, TOK_EQ_EQ, start);
            if (match(lexer, '>')) return make_token(lexer, TOK_FAT_ARROW, start);
            return make_token(lexer, TOK_EQ, start);
        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOK_BANG_EQ, start);
            return make_token(lexer, TOK_BANG, start);
        case '<':
            if (match(lexer, '=')) return make_token(lexer, TOK_LTE, start);
            if (match(lexer, '<')) return make_token(lexer, TOK_LSHIFT, start);
            if (match(lexer, '-')) return make_token(lexer, TOK_LARROW, start);
            return make_token(lexer, TOK_LT, start);
        case '>':
            if (match(lexer, '=')) return make_token(lexer, TOK_GTE, start);
            if (match(lexer, '>')) return make_token(lexer, TOK_RSHIFT, start);
            return make_token(lexer, TOK_GT, start);
        case '&':
            if (match(lexer, '&')) return make_token(lexer, TOK_AND_AND, start);
            return make_token(lexer, TOK_AMP, start);
        case '|':
            if (match(lexer, '|')) return make_token(lexer, TOK_OR_OR, start);
            return make_token(lexer, TOK_PIPE, start);
        case ':':
            if (match(lexer, '=')) return make_token(lexer, TOK_COLON_EQ, start);
            return make_token(lexer, TOK_COLON, start);
        case '"': return string(lexer, start);
    }


    char msg[64];
    snprintf(msg, sizeof(msg), "unexpected character '%c'", c);
    diag_error(lexer->diag, lexer->path, lexer->line, ERR_UNEXPECTED_CHAR, msg);
    return make_token(lexer, TOK_EOF, start);
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOK_EOF: return "EOF";
        case TOK_IDENT: return "IDENT";
        case TOK_INT: return "INT";
        case TOK_FLOAT: return "FLOAT";
        case TOK_STRING: return "STRING";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NONE: return "NONE";
        case TOK_WHILE: return "WHILE";
        case TOK_BREAK: return "BREAK";
        case TOK_DEFER: return "DEFER";
        case TOK_SKIP: return "SKIP";
        case TOK_MOVE: return "MOVE";
        case TOK_UNTIL: return "UNTIL";
        case TOK_DOT_DOT_DOT: return "DOT_DOT_DOT";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_EQ: return "EQ";
        case TOK_EQ_EQ: return "EQ_EQ";
        case TOK_BANG_EQ: return "BANG_EQ";
        case TOK_LT: return "LT";
        case TOK_LTE: return "LTE";
        case TOK_GT: return "GT";
        case TOK_GTE: return "GTE";
        case TOK_AND_AND: return "AND_AND";
        case TOK_OR_OR: return "OR_OR";
        case TOK_BANG: return "BANG";
        case TOK_AMP: return "AMP";
        case TOK_PIPE: return "PIPE";
        case TOK_CARET: return "CARET";
        case TOK_LSHIFT: return "LSHIFT";
        case TOK_RSHIFT: return "RSHIFT";
        case TOK_PLUS_EQ: return "PLUS_EQ";
        case TOK_MINUS_EQ: return "MINUS_EQ";
        case TOK_STAR_EQ: return "STAR_EQ";
        case TOK_SLASH_EQ: return "SLASH_EQ";
        case TOK_PERCENT_EQ: return "PERCENT_EQ";
        case TOK_COLON_EQ: return "COLON_EQ";
        case TOK_LARROW: return "LARROW";
        case TOK_RARROW: return "RARROW";
        case TOK_QUESTION: return "QUESTION";
        case TOK_COLON: return "COLON";
        case TOK_DOT_DOT: return "DOT_DOT";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_COMMA: return "COMMA";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_SPAWN: return "SPAWN";
        case TOK_CHAN: return "CHAN";
        case TOK_MATCH: return "MATCH";
        case TOK_STRUCT: return "STRUCT";
        case TOK_ENUM: return "ENUM";
        case TOK_MUT: return "MUT";
        case TOK_SEMICOLON: return "SEMICOLON";
        case TOK_FAT_ARROW: return "FAT_ARROW";
        case TOK_QUESTION_QUESTION: return "QUESTION_QUESTION";
        case TOK_UNDERSCORE: return "UNDERSCORE";
        case TOK_DOT: return "DOT";
        case TOK_IMPORT: return "IMPORT";
        case TOK_EXTERN: return "EXTERN";
    }
    return "UNKNOWN";
}
