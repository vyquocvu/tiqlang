#define _POSIX_C_SOURCE 200809L
#include "../include/formatter.h"
#include "../include/lexer.h"
#include "../include/diag.h"
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

// One formatting engine writing through a two-function sink (plan 2.3);
// the growable-buffer and FILE * outputs are thin adapters below, so the
// stdin and file code paths can no longer diverge.
typedef struct FmtSink FmtSink;
struct FmtSink {
    void (*write_bytes)(FmtSink *sink, const char *bytes, size_t len);
    void (*write_char)(FmtSink *sink, char c);
};

typedef struct {
    FmtSink *sink;
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
            fmt->sink->write_char(fmt->sink, '\t');
        }
    } else {
        for (int i = 0; i < fmt->pending_indent * fmt->opts->indent_width; i++) {
            fmt->sink->write_char(fmt->sink, ' ');
        }
    }
    fmt->at_line_start = false;
}

static void emit_space(Formatter *fmt) {
    if (!fmt->at_line_start) {
        fmt->sink->write_char(fmt->sink, ' ');
        fmt->col++;
    }
}

static void emit_newline(Formatter *fmt) {
    if (!fmt->at_line_start) {
        fmt->sink->write_char(fmt->sink, '\n');
        fmt->line++;
        fmt->col = 0;
        fmt->at_line_start = true;
    }
}

static void emit_comment(Formatter *fmt, Token token) {
    if (token.comment_length == 0) return;
    if (fmt->at_line_start) {
        ensure_indent(fmt);
    } else {
        emit_space(fmt);
    }
    fmt->sink->write_bytes(fmt->sink, token.comment_start, token.comment_length);
    fmt->col += (int)token.comment_length;
}

static void emit_token(Formatter *fmt, Token token) {
    emit_comment(fmt, token);
    if (token.kind == TOK_NEWLINE) {
        fmt->sink->write_char(fmt->sink, '\n');
        fmt->line++;
        fmt->col = 0;
        fmt->at_line_start = true;
        return;
    }
    ensure_indent(fmt);
    fmt->sink->write_bytes(fmt->sink, token.start, token.length);
    fmt->col += (int)token.length;
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
           kind == TOK_TRUE || kind == TOK_FALSE || kind == TOK_STRUCT ||
           kind == TOK_MATCH || kind == TOK_SPAWN || kind == TOK_CHAN;
}

static void format_stream(FmtSink *sink, Lexer *lexer, FormatterOptions *opts) {
    Formatter fmt = {0};
    fmt.sink = sink;
    fmt.opts = opts;
    fmt.line = 1;
    fmt.at_line_start = true;

    Token current = lexer_next(lexer);
    Token next = lexer_next(lexer);
    bool glue_next_lbrace = false; // "] {" of a bracket loop stays on one line

    while (current.kind != TOK_EOF) {
        Token token = current;
        Token peek = next; // token's immediate successor (lookahead)
        current = next;
        next = lexer_next(lexer);

        // Spacing around operators
        if (is_binary_op(token.kind) || token.kind == TOK_EQ ||
            token.kind == TOK_PLUS_EQ || token.kind == TOK_MINUS_EQ ||
            token.kind == TOK_STAR_EQ || token.kind == TOK_SLASH_EQ ||
            token.kind == TOK_PERCENT_EQ || token.kind == TOK_COLON_EQ ||
            token.kind == TOK_LARROW || token.kind == TOK_RARROW ||
            token.kind == TOK_QUESTION_QUESTION || token.kind == TOK_COLON) {
            emit_token(&fmt, token);
            emit_space(&fmt);
            continue;
        }

        // M8: '?' can be ternary conditional or propagation operator.
        // Ternary: space after. Propagation (followed by newline/EOF/closer): no space.
        if (token.kind == TOK_QUESTION) {
            emit_token(&fmt, token);
            if (peek.kind != TOK_NEWLINE && peek.kind != TOK_EOF &&
                peek.kind != TOK_RBRACE && peek.kind != TOK_RBRACKET &&
                peek.kind != TOK_RPAREN && peek.kind != TOK_COMMA) {
                emit_space(&fmt);
            }
            continue;
        }

        // Braces open/close their own lines and adjust indentation
        if (token.kind == TOK_LBRACE) {
            if (!glue_next_lbrace) emit_newline(&fmt);
            glue_next_lbrace = false;
            fmt.pending_indent++;
            emit_token(&fmt, token);
            emit_newline(&fmt);
            continue;
        }
        if (token.kind == TOK_RBRACE) {
            emit_newline(&fmt);
            fmt.pending_indent--;
            emit_token(&fmt, token);
            emit_newline(&fmt);
            continue;
        }

        // Brackets - newline after [ and before ]
        if (token.kind == TOK_LBRACKET) {
            emit_token(&fmt, token);
            emit_newline(&fmt);
            fmt.pending_indent++;
            continue;
        }
        if (token.kind == TOK_RBRACKET) {
            emit_newline(&fmt);
            fmt.pending_indent--;
            emit_token(&fmt, token);
            if (peek.kind == TOK_LBRACE) {
                emit_space(&fmt);
                glue_next_lbrace = true;
            } else {
                emit_newline(&fmt);
            }
            continue;
        }

        // Commas - space after
        if (token.kind == TOK_COMMA) {
            emit_token(&fmt, token);
            emit_space(&fmt);
            continue;
        }

        if (token.kind == TOK_NEWLINE) {
            emit_token(&fmt, token);
            continue;
        }

        emit_token(&fmt, token);

        // Space after identifiers and literals before non-punctuation
        if (token.kind == TOK_IDENT || token.kind == TOK_INT ||
            token.kind == TOK_FLOAT || token.kind == TOK_STRING ||
            token.kind == TOK_TRUE || token.kind == TOK_FALSE) {
            if (peek.kind != TOK_NEWLINE && peek.kind != TOK_EOF &&
                peek.kind != TOK_RBRACE && peek.kind != TOK_RBRACKET &&
                peek.kind != TOK_RPAREN && peek.kind != TOK_COMMA &&
                peek.kind != TOK_LPAREN && peek.kind != TOK_DOT &&
                !is_binary_op(peek.kind) && peek.kind != TOK_QUESTION &&
                peek.kind != TOK_COLON) {
                emit_space(&fmt);
            }
            // M12.6: Glue '{' after identifier for struct defs and record literals
            if (peek.kind == TOK_LBRACE) {
                glue_next_lbrace = true;
            }
        }

        // Space after keywords, move, and range/ellipsis operators
        if (token_is_keyword(token.kind)) emit_space(&fmt);
        if (token.kind == TOK_MOVE) emit_space(&fmt);
        if (token.kind == TOK_DOT_DOT_DOT) emit_space(&fmt);
        if (token.kind == TOK_DOT_DOT) emit_space(&fmt);
    }

    // A comment before EOF (no trailing newline) attaches to the EOF token.
    emit_comment(&fmt, current);

    if (opts->insert_final_newline && !fmt.at_line_start) {
        fmt.sink->write_char(fmt.sink, '\n');
    }
}

// ---------------------------------------------------------------- buffer sink

typedef struct {
    FmtSink sink;
    char *data;
    size_t cap;
    size_t len;
} BufSink;

static void buf_ensure(BufSink *bs, size_t extra) {
    if (bs->len + extra > bs->cap) {
        size_t need = bs->len + extra;
        bs->cap = bs->cap < 4096 ? 4096 : bs->cap * 2;
        while (bs->cap < need) bs->cap *= 2;
        bs->data = realloc(bs->data, bs->cap);
        if (!bs->data) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
}

static void buf_write_bytes(FmtSink *sink, const char *bytes, size_t len) {
    BufSink *bs = (BufSink *)sink;
    buf_ensure(bs, len);
    memcpy(bs->data + bs->len, bytes, len);
    bs->len += len;
}

static void buf_write_char(FmtSink *sink, char c) {
    BufSink *bs = (BufSink *)sink;
    buf_ensure(bs, 1);
    bs->data[bs->len++] = c;
}

char *format_source(const char *source, const char *path, FormatterOptions *opts) {
    DiagContext diag;
    diag_init(&diag);
    Lexer lexer;
    lexer_init(&lexer, source, path, &diag);

    BufSink bs = { { buf_write_bytes, buf_write_char }, NULL, 0, 0 };
    format_stream(&bs.sink, &lexer, opts);
    buf_ensure(&bs, 1);
    bs.data[bs.len] = '\0';
    return bs.data;
}

// ---------------------------------------------------------------- FILE sink

typedef struct {
    FmtSink sink;
    FILE *out;
} FileSink;

static void file_write_bytes(FmtSink *sink, const char *bytes, size_t len) {
    fwrite(bytes, 1, len, ((FileSink *)sink)->out);
}

static void file_write_char(FmtSink *sink, char c) {
    fputc(c, ((FileSink *)sink)->out);
}

// ---------------------------------------------------------------- entry points

int format_file(const char *input, const char *output, FormatterOptions *opts) {
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

    char *formatted = format_source(source, input, opts);
    int result = 0;
    if (output == NULL) {
        fputs(formatted, stdout);
    } else {
        FILE *out = fopen(output, "wb");
        if (!out) { result = 1; }
        else { fputs(formatted, out); fclose(out); }
    }
    free(formatted);
    free(source);
    return result;
}

int format_stdin_to_file(const char *output, FormatterOptions *opts) {
    // Read all from stdin
    char *source = NULL;
    size_t capacity = 0;
    size_t size = 0;
    int c;

    while ((c = fgetc(stdin)) != EOF) {
        if (size + 1 >= capacity) {
            capacity = capacity == 0 ? 4096 : capacity * 2;
            char *new_source = realloc(source, capacity);
            if (!new_source) { free(source); return 1; }
            source = new_source;
        }
        source[size++] = (char)c;
    }
    if (ferror(stdin)) { free(source); return 1; }
    if (source) source[size] = '\0';

    FILE *out;
    if (output == NULL) {
        out = stdout;
    } else {
        out = fopen(output, "wb");
        if (!out) { free(source); return 1; }
    }

    DiagContext diag;
    diag_init(&diag);
    Lexer lexer;
    lexer_init(&lexer, source ? source : "", "(stdin)", &diag);

    FileSink fs = { { file_write_bytes, file_write_char }, out };
    format_stream(&fs.sink, &lexer, opts);

    if (output != NULL) fclose(out);
    free(source);
    return diag.has_error ? 1 : 0;
}
