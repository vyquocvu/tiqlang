#define _POSIX_C_SOURCE 200809L
#include "../include/formatter.h"
#include "../include/lexer.h"
#include "../include/diag.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

void formatter_init_options(FormatterOptions *opts) {
    opts->use_tabs = false;
    opts->indent_width = 4;
    opts->max_line_length = 100;
    opts->insert_final_newline = true;
}

typedef struct {
    FILE *out;
    FormatterOptions *opts;
    int line;
    int col;
    int pending_indent;
    bool at_line_start;
} Formatter;

static void ensure_indent(Formatter *fmt) {
    if (!fmt->at_line_start) return;
    if (fmt->opts->use_tabs) {
        for (int i = 0; i < fmt->pending_indent; i++) {
            fputc('\t', fmt->out);
        }
    } else {
        for (int i = 0; i < fmt->pending_indent * fmt->opts->indent_width; i++) {
            fputc(' ', fmt->out);
        }
    }
    fmt->at_line_start = false;
}

static void emit_token(Formatter *fmt, Token token) {
    if (token.kind == TOK_NEWLINE) {
        fputc('\n', fmt->out);
        fmt->line++;
        fmt->col = 0;
        fmt->at_line_start = true;
        return;
    }
    ensure_indent(fmt);
    for (size_t i = 0; i < token.length; i++) {
        fputc(token.start[i], fmt->out);
        fmt->col++;
    }
}

static void emit_space(Formatter *fmt) {
    if (!fmt->at_line_start) {
        fputc(' ', fmt->out);
        fmt->col++;
    }
}

static void emit_newline(Formatter *fmt) {
    if (!fmt->at_line_start) {
        fputc('\n', fmt->out);
        fmt->line++;
        fmt->col = 0;
        fmt->at_line_start = true;
    }
}

static bool is_binary_op(TokenKind kind) {
    return kind == TOK_PLUS || kind == TOK_MINUS || kind == TOK_STAR ||
           kind == TOK_SLASH || kind == TOK_PERCENT || kind == TOK_EQ_EQ ||
           kind == TOK_BANG_EQ || kind == TOK_LT || kind == TOK_LTE ||
           kind == TOK_GT || kind == TOK_GTE || kind == TOK_AND_AND ||
           kind == TOK_OR_OR || kind == TOK_AMP || kind == TOK_PIPE ||
           kind == TOK_CARET || kind == TOK_LSHIFT || kind == TOK_RSHIFT;
}

static bool token_is_keyword(TokenKind kind) {
    return kind == TOK_WHILE || kind == TOK_BREAK || kind == TOK_SKIP ||
           kind == TOK_MOVE || kind == TOK_DEFER || kind == TOK_UNTIL ||
           kind == TOK_TRUE || kind == TOK_FALSE;
}

static void format_with_lexer(Formatter *fmt, Lexer *lexer) {
    Token current = lexer_next(lexer);
    Token next = lexer_next(lexer);

    while (current.kind != TOK_EOF) {
        Token token = current;
        Token peek = next;

        // Advance lexer for next iteration
        current = next;
        next = lexer_next(lexer);

        // Handle spacing around operators
        if (is_binary_op(token.kind) || token.kind == TOK_EQ ||
            token.kind == TOK_PLUS_EQ || token.kind == TOK_MINUS_EQ ||
            token.kind == TOK_STAR_EQ || token.kind == TOK_SLASH_EQ ||
            token.kind == TOK_PERCENT_EQ || token.kind == TOK_COLON_EQ ||
            token.kind == TOK_LARROW || token.kind == TOK_RARROW ||
            token.kind == TOK_QUESTION || token.kind == TOK_COLON) {
            emit_token(fmt, token);
            emit_space(fmt);
            continue;
        }

        // Handle braces
        if (token.kind == TOK_LBRACE) {
            emit_newline(fmt);
            fmt->pending_indent++;
            emit_token(fmt, token);
            emit_newline(fmt);
            continue;
        }
        if (token.kind == TOK_RBRACE) {
            emit_newline(fmt);
            fmt->pending_indent--;
            emit_token(fmt, token);
            emit_newline(fmt);
            continue;
        }

        // Handle brackets - newlines after [ and before ]
        if (token.kind == TOK_LBRACKET) {
            emit_token(fmt, token);
            emit_newline(fmt);
            fmt->pending_indent++;
            continue;
        }
        if (token.kind == TOK_RBRACKET) {
            emit_newline(fmt);
            fmt->pending_indent--;
            emit_token(fmt, token);
            emit_newline(fmt);
            continue;
        }

        // Handle commas - space after, newline before
        if (token.kind == TOK_COMMA) {
            emit_token(fmt, token);
            emit_space(fmt);
            continue;
        }

        // Handle newlines
        if (token.kind == TOK_NEWLINE) {
            emit_token(fmt, token);
            continue;
        }

        // Default: emit token with space after if needed
        emit_token(fmt, token);

        // Add space after identifiers and literals before non-punctuation
        if (token.kind == TOK_IDENT || token.kind == TOK_INT ||
            token.kind == TOK_FLOAT || token.kind == TOK_STRING ||
            token.kind == TOK_TRUE || token.kind == TOK_FALSE) {
            if (peek.kind != TOK_NEWLINE && peek.kind != TOK_EOF &&
                peek.kind != TOK_RBRACE && peek.kind != TOK_RBRACKET &&
                peek.kind != TOK_RPAREN && peek.kind != TOK_COMMA &&
                !is_binary_op(peek.kind) && peek.kind != TOK_QUESTION &&
                peek.kind != TOK_COLON) {
                emit_space(fmt);
            }
        }

        // Add space after keywords
        if (token_is_keyword(token.kind)) {
            emit_space(fmt);
        }

        // Add space after print and move
        if ((token.kind == TOK_BANG) || (token.kind == TOK_MOVE)) {
            emit_space(fmt);
        }

        // Handle DOT_DOT_DOT specially (stream generator ellipsis)
        if (token.kind == TOK_DOT_DOT_DOT) {
            emit_space(fmt);
        }

        // Handle DOT_DOT (range operator)
        if (token.kind == TOK_DOT_DOT) {
            emit_space(fmt);
        }
    }
}

int format_file(const char *input, const char *output, FormatterOptions *opts) {
    // Read input file
    FILE *in = fopen(input, "rb");
    if (!in) {
        fprintf(stderr, "tiq: cannot open %s: %s\n", input, strerror(errno));
        return 1;
    }
    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return 1; }
    long size = ftell(in);
    if (size < 0) { fclose(in); return 1; }
    if (fseek(in, 0, SEEK_SET) != 0) { fclose(in); return 1; }

    char *source = malloc((size_t)size + 1);
    if (!source) { fclose(in); return 1; }
    if (fread(source, 1, (size_t)size, in) != (size_t)size) { free(source); fclose(in); return 1; }
    source[size] = '\0';
    fclose(in);

    // Open output file
    FILE *out;
    if (output == NULL) {
        out = stdout;
    } else {
        out = fopen(output, "wb");
        if (!out) { free(source); return 1; }
    }

    // Format
    DiagContext diag;
    diag_init(&diag);
    Lexer lexer;
    lexer_init(&lexer, source, input, &diag);

    Formatter fmt = {0};
    fmt.out = out;
    fmt.opts = opts;
    fmt.line = 1;
    fmt.col = 0;
    fmt.pending_indent = 0;
    fmt.at_line_start = true;

    format_with_lexer(&fmt, &lexer);

    // Ensure final newline
    if (opts->insert_final_newline) {
        if (!fmt.at_line_start) {
            fputc('\n', out);
        }
    }

    if (output != NULL) fclose(out);
    free(source);
    return diag.has_error ? 1 : 0;
}

int format_stdin_to_file(const char *output, FormatterOptions *opts) {
    // Read all from stdin
    char *source = NULL;
    size_t capacity = 0;
    size_t size = 0;
    int c;

    while ((c = fgetc(stdin)) != EOF) {
        if (size >= capacity) {
            capacity = capacity == 0 ? 4096 : capacity * 2;
            char *new_source = realloc(source, capacity);
            if (!new_source) { free(source); return 1; }
            source = new_source;
        }
        source[size++] = (char)c;
    }
    if (ferror(stdin)) { free(source); return 1; }
    if (source) source[size] = '\0';

    // Open output file
    FILE *out;
    if (output == NULL) {
        out = stdout;
    } else {
        out = fopen(output, "wb");
        if (!out) { free(source); return 1; }
    }

    // Format
    DiagContext diag;
    diag_init(&diag);
    Lexer lexer;
    lexer_init(&lexer, source ? source : "", "(stdin)", &diag);

    Formatter fmt = {0};
    fmt.out = out;
    fmt.opts = opts;
    fmt.line = 1;
    fmt.col = 0;
    fmt.pending_indent = 0;
    fmt.at_line_start = true;

    format_with_lexer(&fmt, &lexer);

    // Ensure final newline
    if (opts->insert_final_newline) {
        if (!fmt.at_line_start) {
            fputc('\n', out);
        }
    }

    if (output != NULL) fclose(out);
    free(source);
    return diag.has_error ? 1 : 0;
}
