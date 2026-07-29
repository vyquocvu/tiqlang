// LSP server over JSON-RPC/stdio (plan 5.1). The compiler front end is
// callable as a library, so hover/definition/semanticTokens answer with
// real symbol data: didOpen stores the document text, each request runs
// lexer+parser+semantic on the stored text, and responses are
// deterministic for a given (uri, version). Unsupported or malformed
// requests fail closed with a null result.
#include "../include/lsp.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/type.h"
#include "../include/diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LSP_MAX_DOCS 8

typedef struct {
    char uri[512];
    long version;
    char *text; // malloc'd, JSON-unescaped source
    bool open;
} LspDocument;

typedef struct {
    char root_path[1024];
    LspDocument docs[LSP_MAX_DOCS];
} LspServer;

// ============================================================================
// JSON-RPC framing
// ============================================================================

static char *read_message(FILE *in) {
    // Read header lines up to the blank separator; both \r\n and bare \n
    // line endings are accepted, anything without a positive
    // Content-Length fails closed.
    char header[256];
    size_t header_len = 0;
    long content_length = -1;
    bool saw_blank = false;
    int c;

    while ((c = fgetc(in)) != EOF) {
        if (c == '\n') {
            if (header_len > 0 && header[header_len - 1] == '\r') header_len--;
            header[header_len] = '\0';
            if (header_len == 0) { saw_blank = true; break; }
            if (strncmp(header, "Content-Length", 14) == 0) {
                char *colon = strchr(header, ':');
                if (colon) {
                    while (*++colon == ' ');
                    content_length = atol(colon);
                }
            }
            header_len = 0;
            continue;
        }
        if (header_len < sizeof(header) - 1) {
            header[header_len++] = (char)c;
        }
    }

    if (!saw_blank || content_length <= 0) return NULL;

    // Read content
    char *content = malloc((size_t)content_length + 1);
    if (!content) return NULL;
    size_t total = 0;
    while (total < (size_t)content_length && (c = fgetc(in)) != EOF) {
        content[total++] = (char)c;
    }
    content[total] = '\0';

    return content;
}

static void send_message(FILE *out, const char *content) {
    fprintf(out, "Content-Length: %zu\r\n\r\n%s", strlen(content), content);
    fflush(out);
}

// Envelope buffers are sized from the payload, so semantic token data for
// large documents is never truncated mid-message.
static void send_response(FILE *out, long id, const char *result) {
    size_t cap = strlen(result) + 64;
    char *content = malloc(cap);
    if (!content) return;
    snprintf(content, cap, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}",
             id, result);
    send_message(out, content);
    free(content);
}

static void send_notification(FILE *out, const char *method, const char *params) {
    size_t cap = strlen(method) + strlen(params) + 64;
    char *content = malloc(cap);
    if (!content) return;
    snprintf(content, cap, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
             method, params);
    send_message(out, content);
    free(content);
}

// ============================================================================
// Minimal JSON extraction (flat key scan; enough for the LSP requests we
// accept, anything else fails closed by returning NULL/false)
// ============================================================================

// Return the position just after `"key":` (spaces skipped), or NULL.
static const char *json_after_key(const char *json, const char *key) {
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern))
        return NULL;
    const char *at = strstr(json, pattern);
    if (!at) return NULL;
    const char *colon = strchr(at + strlen(pattern), ':');
    if (!colon) return NULL;
    colon++;
    while (*colon == ' ') colon++;
    return colon;
}

// Unescape the JSON string starting at the opening quote `src`. Returns a
// malloc'd C string or NULL on malformed input (fail closed).
static char *json_unescape(const char *src) {
    if (*src != '"') return NULL;
    src++;
    size_t cap = strlen(src) + 1; // unescaping never grows the text
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    while (*src && *src != '"') {
        char c = *src++;
        if (c == '\\') {
            char e = *src++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'u':
                    // Non-ASCII escapes are outside the bootstrap scope.
                    for (int i = 0; i < 4 && *src; i++) src++;
                    c = '?';
                    break;
                default:
                    free(out);
                    return NULL;
            }
        }
        out[len++] = c;
    }
    if (*src != '"') { // unterminated string
        free(out);
        return NULL;
    }
    out[len] = '\0';
    return out;
}

static char *json_get_string(const char *json, const char *key) {
    const char *v = json_after_key(json, key);
    if (!v) return NULL;
    return json_unescape(v);
}

static bool json_get_long(const char *json, const char *key, long *out) {
    const char *v = json_after_key(json, key);
    if (!v) return false;
    char *end = NULL;
    long value = strtol(v, &end, 10);
    if (end == v) return false;
    *out = value;
    return true;
}

// ============================================================================
// Document store
// ============================================================================

static LspDocument *doc_find(LspServer *server, const char *uri) {
    for (int i = 0; i < LSP_MAX_DOCS; i++) {
        if (server->docs[i].open && strcmp(server->docs[i].uri, uri) == 0)
            return &server->docs[i];
    }
    return NULL;
}

// Takes ownership of `text`; drops the document (fail closed) when the
// store is full or the uri does not fit.
static void doc_open(LspServer *server, const char *uri, long version, char *text) {
    if (strlen(uri) >= sizeof(server->docs[0].uri)) {
        free(text);
        return;
    }
    LspDocument *doc = doc_find(server, uri);
    if (!doc) {
        for (int i = 0; i < LSP_MAX_DOCS; i++) {
            if (!server->docs[i].open) { doc = &server->docs[i]; break; }
        }
    }
    if (!doc) {
        free(text);
        return;
    }
    free(doc->text);
    snprintf(doc->uri, sizeof(doc->uri), "%s", uri);
    doc->version = version;
    doc->text = text;
    doc->open = true;
}

static void docs_free(LspServer *server) {
    for (int i = 0; i < LSP_MAX_DOCS; i++) {
        free(server->docs[i].text);
        server->docs[i].text = NULL;
        server->docs[i].open = false;
    }
}

// ============================================================================
// Source positions (LSP is 0-based; lexer lines are 1-based)
// ============================================================================

static int token_col(const char *text, const Token *tok) {
    const char *p = tok->start;
    while (p > text && p[-1] != '\n') p--;
    return (int)(tok->start - p);
}

// Find the identifier token covering (line, character); false when the
// position does not name an identifier.
static bool find_ident_at(const char *text, long line, long character, Token *out) {
    Lexer lexer;
    DiagContext diag;
    diag_init(&diag);
    lexer_init(&lexer, text, "<lsp>", &diag);
    for (;;) {
        Token tok = lexer_next(&lexer);
        if (tok.kind == TOK_EOF) break;
        if (tok.kind != TOK_IDENT) continue;
        if ((long)tok.line - 1 != line) continue;
        long col = token_col(text, &tok);
        if (character >= col && character < col + (long)tok.length) {
            *out = tok;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Declaration index: walk the checked AST and record every name a hover
// or definition request can resolve to
// ============================================================================

typedef struct {
    Token name;
    const SemanticType *type;
    bool is_function;
    int param_count;
} LspDecl;

#define LSP_MAX_DECLS 512

typedef struct {
    LspDecl items[LSP_MAX_DECLS];
    int count;
} LspDeclList;

static void decl_add(LspDeclList *list, Token name, const void *type,
                     bool is_function, int param_count) {
    if (list->count >= LSP_MAX_DECLS) return; // fail closed: extra decls unresolvable
    LspDecl *d = &list->items[list->count++];
    d->name = name;
    d->type = (const SemanticType *)type;
    d->is_function = is_function;
    d->param_count = param_count;
}

static void collect_decls(LspDeclList *list, const AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case AST_BINDING:
            decl_add(list, node->as.binding.name,
                     node->as.binding.expr ? node->as.binding.expr->semantic_type : NULL,
                     false, 0);
            collect_decls(list, node->as.binding.expr);
            break;
        case AST_ASSIGN:
            if (node->as.assign.is_definition) {
                decl_add(list, node->as.assign.name,
                         node->as.assign.expr ? node->as.assign.expr->semantic_type : NULL,
                         false, 0);
            }
            collect_decls(list, node->as.assign.index);
            collect_decls(list, node->as.assign.expr);
            break;
        case AST_FUNCTION:
            decl_add(list, node->as.function.name, node->semantic_type,
                     true, node->as.function.param_count);
            for (int i = 0; i < node->as.function.param_count; i++) {
                decl_add(list, node->as.function.params[i],
                         node->as.function.param_types ? node->as.function.param_types[i] : NULL,
                         false, 0);
            }
            collect_decls(list, node->as.function.body);
            break;
        case AST_BINARY:
            collect_decls(list, node->as.binary.left);
            collect_decls(list, node->as.binary.right);
            break;
        case AST_UNARY:
            collect_decls(list, node->as.unary.right);
            break;
        case AST_CONDITIONAL:
            collect_decls(list, node->as.conditional.cond);
            collect_decls(list, node->as.conditional.then_branch);
            collect_decls(list, node->as.conditional.else_branch);
            break;
        case AST_CALL:
            collect_decls(list, node->as.call.callee);
            for (int i = 0; i < node->as.call.arg_count; i++)
                collect_decls(list, node->as.call.args[i]);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.stmt_count; i++)
                collect_decls(list, node->as.block.statements[i]);
            collect_decls(list, node->as.block.final_expr);
            for (int i = 0; i < node->as.block.defer_count; i++)
                collect_decls(list, node->as.block.deferred[i]);
            break;
        case AST_BRACKET_LOOP:
            collect_decls(list, node->as.bracket_loop.domain);
            for (int i = 0; i < node->as.bracket_loop.body_count; i++)
                collect_decls(list, node->as.bracket_loop.body_stmts[i]);
            collect_decls(list, node->as.bracket_loop.body_final);
            break;
        case AST_STREAM_GEN:
            for (int i = 0; i < node->as.stream_gen.seed_count; i++)
                collect_decls(list, node->as.stream_gen.seeds[i]);
            collect_decls(list, node->as.stream_gen.gen_expr);
            collect_decls(list, node->as.stream_gen.bound);
            break;
        case AST_ARRAY:
            for (int i = 0; i < node->as.array.element_count; i++)
                collect_decls(list, node->as.array.elements[i]);
            break;
        case AST_ARRAY_FILL:
            collect_decls(list, node->as.array_fill.value);
            collect_decls(list, node->as.array_fill.length);
            break;
        case AST_FIELD_ACCESS:
            collect_decls(list, node->as.field_access.target);
            break;
        case AST_RECORD_LIT:
            for (int i = 0; i < node->as.record_lit.field_count; i++)
                collect_decls(list, node->as.record_lit.field_values[i]);
            break;
        case AST_MATCH:
            collect_decls(list, node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                collect_decls(list, node->as.match_expr.arms[i].pattern);
                collect_decls(list, node->as.match_expr.arms[i].body);
            }
            break;
        case AST_SPAWN:
            collect_decls(list, node->as.spawn.expr);
            break;
        case AST_DEFER:
            collect_decls(list, node->as.defer.expr);
            break;
        default:
            break; // literals, identifiers, break/skip, struct defs, chan
    }
}

static bool token_eq(const Token *a, const Token *b) {
    return a->length == b->length && memcmp(a->start, b->start, a->length) == 0;
}

// Scope approximation over the collected declarations: prefer the closest
// declaration at or before the use site; fall back to the first later one
// so forward-referenced functions still resolve.
static const LspDecl *resolve_decl(const LspDeclList *list, const char *text,
                                   const Token *use) {
    const LspDecl *best = NULL;
    const LspDecl *fallback = NULL;
    int use_col = token_col(text, use);
    for (int i = 0; i < list->count; i++) {
        const LspDecl *d = &list->items[i];
        if (!token_eq(&d->name, use)) continue;
        int dcol = token_col(text, &d->name);
        if (d->name.line < use->line ||
            (d->name.line == use->line && dcol <= use_col)) {
            best = d;
        } else if (!fallback) {
            fallback = d;
        }
    }
    return best ? best : fallback;
}

// ============================================================================
// Front-end run shared by hover and definition: locate the identifier at
// the request position and its declaration in the checked AST. The parser
// (and every AST node) stays alive in `parser` until the caller frees it.
// ============================================================================

typedef struct {
    Parser parser;
    TypePool pool;
    Token word;
    const LspDecl *decl;
    LspDeclList decls;
} LspLookup;

static bool lookup_at(LspLookup *lk, const LspDocument *doc, long line, long character) {
    if (!find_ident_at(doc->text, line, character, &lk->word)) return false;
    DiagContext diag;
    diag_init(&diag);
    parser_init(&lk->parser, doc->text, doc->uri, &diag);
    int count;
    AstNode **stmts = parser_parse(&lk->parser, &count);
    type_pool_init(&lk->pool);
    if (!diag.has_error) semantic_check(stmts, count, doc->uri, &diag, &lk->pool);
    lk->decls.count = 0;
    for (int i = 0; i < count; i++) collect_decls(&lk->decls, stmts[i]);
    lk->decl = resolve_decl(&lk->decls, doc->text, &lk->word);
    return lk->decl != NULL;
}

static void lookup_free(LspLookup *lk) {
    parser_free(&lk->parser);
    type_pool_free(&lk->pool);
}

// ============================================================================
// Request handlers
// ============================================================================

static void handle_initialize(FILE *out, long id) {
    // The semantic token legend is part of the observable protocol; the
    // indices below must match token_type_index().
    const char *capabilities =
        "{\"capabilities\":{"
        "\"textDocumentSync\":1,"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"semanticTokensProvider\":{"
        "\"legend\":{"
        "\"tokenTypes\":[\"keyword\",\"variable\",\"number\",\"string\",\"operator\"],"
        "\"tokenModifiers\":[]"
        "},"
        "\"full\":true"
        "}"
        "},"
        "\"serverInfo\":{\"name\":\"tiq\",\"version\":\"0.1.0\"}}";
    send_response(out, id, capabilities);
}

static void handle_shutdown(FILE *out, long id) {
    send_response(out, id, "null");
}

// M11.1: structured in-protocol diagnostics. Run the front end over the
// stored document with a bounded DiagRecord sink and publish each record
// as an LSP diagnostic: 0-based start-of-line range, severity 1 (Error),
// code "ENN", source "tiq". Records past the sink cap are dropped by the
// sink itself, so the payload stays bounded.
#define LSP_MAX_DIAGS 16

static void append_json_escaped(char *dst, size_t cap, size_t *len, const char *s) {
    for (; *s; s++) {
        char buf[8];
        int n;
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            n = snprintf(buf, sizeof(buf), "\\%c", c);
        } else if (c < 0x20) {
            n = snprintf(buf, sizeof(buf), "\\u%04x", c);
        } else {
            buf[0] = (char)c;
            buf[1] = '\0';
            n = 1;
        }
        if (*len + (size_t)n >= cap) return; // truncate: never overflow
        memcpy(dst + *len, buf, (size_t)n);
        *len += (size_t)n;
        dst[*len] = '\0';
    }
}

static void publish_diagnostics(FILE *out, const LspDocument *doc) {
    DiagRecord records[LSP_MAX_DIAGS];
    DiagContext diag;
    diag_init(&diag);
    diag.records = records;
    diag.record_cap = LSP_MAX_DIAGS;

    Parser parser;
    parser_init(&parser, doc->text, doc->uri, &diag);
    int count;
    AstNode **stmts = parser_parse(&parser, &count);
    TypePool pool;
    type_pool_init(&pool);
    if (!diag.has_error) semantic_check(stmts, count, doc->uri, &diag, &pool);

    char params[8192];
    size_t len = 0;
    int n = snprintf(params, sizeof(params),
                     "{\"uri\":\"%s\",\"version\":%ld,\"diagnostics\":[",
                     doc->uri, doc->version);
    if (n < 0 || (size_t)n >= sizeof(params)) n = 0; // fail closed: empty prefix
    len = (size_t)n;
    for (int i = 0; i < diag.record_count; i++) {
        int line0 = records[i].line > 0 ? records[i].line - 1 : 0;
        char head[160];
        int hn = snprintf(head, sizeof(head),
                          "%s{\"range\":{\"start\":{\"line\":%d,\"character\":0},"
                          "\"end\":{\"line\":%d,\"character\":0}},"
                          "\"severity\":1,\"code\":\"E%02d\",\"source\":\"tiq\","
                          "\"message\":\"",
                          i > 0 ? "," : "", line0, line0, (int)records[i].code);
        if (hn < 0 || len + (size_t)hn + 3 >= sizeof(params)) break;
        memcpy(params + len, head, (size_t)hn);
        len += (size_t)hn;
        params[len] = '\0';
        append_json_escaped(params, sizeof(params) - 3, &len, records[i].message);
        params[len++] = '"';
        params[len++] = '}';
        params[len] = '\0';
    }
    if (len + 3 < sizeof(params)) {
        params[len++] = ']';
        params[len++] = '}';
        params[len] = '\0';
        send_notification(out, "textDocument/publishDiagnostics", params);
    }

    parser_free(&parser);
    type_pool_free(&pool);
}

static void handle_did_open(LspServer *server, FILE *out, const char *content) {
    char *uri = json_get_string(content, "uri");
    char *text = json_get_string(content, "text");
    long version = 0;
    if (!uri || !text || !json_get_long(content, "version", &version)) {
        free(uri);
        free(text);
        return; // malformed didOpen: fail closed
    }
    doc_open(server, uri, version, text); // takes ownership of text

    // Publish real front-end diagnostics for the stored document (M11.1);
    // a clean document publishes the empty set.
    LspDocument *doc = doc_find(server, uri);
    if (doc) publish_diagnostics(out, doc);
    free(uri);
}

// Shared param parsing for hover/definition; returns the target document
// or NULL (fail closed → null result).
static LspDocument *request_doc_at(LspServer *server, const char *content,
                                   long *line, long *character) {
    char *uri = json_get_string(content, "uri");
    if (!uri) return NULL;
    LspDocument *doc = doc_find(server, uri);
    free(uri);
    if (!doc) return NULL;
    if (!json_get_long(content, "line", line)) return NULL;
    if (!json_get_long(content, "character", character)) return NULL;
    return doc;
}

static void handle_hover(LspServer *server, FILE *out, long id, const char *content) {
    long line = 0, character = 0;
    LspDocument *doc = request_doc_at(server, content, &line, &character);
    LspLookup lk;
    if (!doc || !lookup_at(&lk, doc, line, character)) {
        send_response(out, id, "null");
        return;
    }
    char tyname[160];
    if (lk.decl->is_function) {
        char ret[96] = "";
        type_display(lk.decl->type, ret, sizeof(ret));
        snprintf(tyname, sizeof(tyname), "fn(%d) -> %s",
                 lk.decl->param_count, ret[0] ? ret : "unknown");
    } else {
        type_display(lk.decl->type, tyname, sizeof(tyname));
        if (!tyname[0]) snprintf(tyname, sizeof(tyname), "unknown");
    }
    long col = token_col(doc->text, &lk.word);
    char result[1024];
    snprintf(result, sizeof(result),
             "{\"contents\":{\"kind\":\"markdown\","
             "\"value\":\"```tiq\\n%.*s: %s\\n```\"},"
             "\"range\":{\"start\":{\"line\":%ld,\"character\":%ld},"
             "\"end\":{\"line\":%ld,\"character\":%ld}}}",
             (int)lk.word.length, lk.word.start, tyname,
             line, col, line, col + (long)lk.word.length);
    send_response(out, id, result);
    lookup_free(&lk);
}

static void handle_definition(LspServer *server, FILE *out, long id, const char *content) {
    long line = 0, character = 0;
    LspDocument *doc = request_doc_at(server, content, &line, &character);
    LspLookup lk;
    if (!doc || !lookup_at(&lk, doc, line, character)) {
        send_response(out, id, "null");
        return;
    }
    long decl_line = lk.decl->name.line - 1;
    long decl_col = token_col(doc->text, &lk.decl->name);
    char result[1024];
    snprintf(result, sizeof(result),
             "{\"uri\":\"%s\",\"range\":{"
             "\"start\":{\"line\":%ld,\"character\":%ld},"
             "\"end\":{\"line\":%ld,\"character\":%ld}}}",
             doc->uri, decl_line, decl_col,
             decl_line, decl_col + (long)lk.decl->name.length);
    send_response(out, id, result);
    lookup_free(&lk);
}

// Legend index for a lexer token kind, or -1 for punctuation/layout that
// the semantic token stream omits. Must match the initialize legend.
static int token_type_index(TokenKind kind) {
    switch (kind) {
        case TOK_TRUE: case TOK_FALSE: case TOK_WHILE: case TOK_BREAK:
        case TOK_SKIP: case TOK_MOVE: case TOK_DEFER: case TOK_UNTIL:
        case TOK_SPAWN: case TOK_CHAN: case TOK_MATCH: case TOK_STRUCT:
        case TOK_MUT:
            return 0; // keyword
        case TOK_IDENT:
            return 1; // variable
        case TOK_INT: case TOK_FLOAT:
            return 2; // number
        case TOK_STRING:
            return 3; // string
        case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH:
        case TOK_PERCENT: case TOK_EQ: case TOK_EQ_EQ: case TOK_BANG_EQ:
        case TOK_LT: case TOK_LTE: case TOK_GT: case TOK_GTE:
        case TOK_AND_AND: case TOK_OR_OR: case TOK_BANG: case TOK_AMP:
        case TOK_PIPE: case TOK_CARET: case TOK_LSHIFT: case TOK_RSHIFT:
        case TOK_PLUS_EQ: case TOK_MINUS_EQ: case TOK_STAR_EQ:
        case TOK_SLASH_EQ: case TOK_PERCENT_EQ: case TOK_COLON_EQ:
        case TOK_LARROW: case TOK_RARROW: case TOK_QUESTION: case TOK_COLON:
        case TOK_DOT_DOT: case TOK_DOT_DOT_DOT: case TOK_FAT_ARROW:
        case TOK_QUESTION_QUESTION:
            return 4; // operator
        default:
            return -1;
    }
}

static void handle_semantic_tokens(LspServer *server, FILE *out, long id,
                                   const char *content) {
    char *uri = json_get_string(content, "uri");
    LspDocument *doc = uri ? doc_find(server, uri) : NULL;
    free(uri);
    if (!doc) {
        send_response(out, id, "null");
        return;
    }

    // LSP delta encoding: [deltaLine, deltaStartChar, length, type, mods].
    size_t cap = 256;
    size_t len = 0;
    char *data = malloc(cap);
    if (!data) {
        send_response(out, id, "null");
        return;
    }
    data[0] = '\0';

    Lexer lexer;
    DiagContext diag;
    diag_init(&diag);
    lexer_init(&lexer, doc->text, doc->uri, &diag);
    int prev_line = 0;
    int prev_col = 0;
    for (;;) {
        Token tok = lexer_next(&lexer);
        if (tok.kind == TOK_EOF) break;
        int type = token_type_index(tok.kind);
        if (type < 0) continue;
        int line = tok.line - 1;
        int col = token_col(doc->text, &tok);
        int delta_line = line - prev_line;
        int delta_col = delta_line == 0 ? col - prev_col : col;
        char entry[96];
        int n = snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d,0",
                         len > 0 ? "," : "", delta_line, delta_col,
                         (int)tok.length, type);
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *grown = realloc(data, cap);
            if (!grown) {
                free(data);
                send_response(out, id, "null");
                return;
            }
            data = grown;
        }
        memcpy(data + len, entry, (size_t)n + 1);
        len += (size_t)n;
        prev_line = line;
        prev_col = col;
    }

    size_t result_cap = len + 32;
    char *result = malloc(result_cap);
    if (result) {
        snprintf(result, result_cap, "{\"data\":[%s]}", data);
        send_response(out, id, result);
        free(result);
    }
    free(data);
}

// ============================================================================
// Dispatch loop
// ============================================================================

static long parse_request_id(const char *content) {
    const char *id_str = strstr(content, "\"id\":");
    if (!id_str) return -1;
    id_str += 5;
    while (*id_str == ' ') id_str++;
    if (*id_str == '"') id_str++;
    return atol(id_str);
}

static int is_request(const char *content) {
    return strstr(content, "\"method\"") != NULL;
}

static char *extract_method(const char *content) {
    const char *m = strstr(content, "\"method\"");
    if (!m) return NULL;
    const char *colon = strchr(m, ':');
    if (!colon) return NULL;
    colon++;
    while (*colon == ' ') colon++;
    if (*colon != '"') return NULL;
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return NULL;
    size_t len = (size_t)(end - colon);
    char *method = malloc(len + 1);
    if (!method) return NULL;
    memcpy(method, colon, len);
    method[len] = '\0';
    return method;
}

int lsp_server_run(const char *root_path, int stdin_fd, int stdout_fd) {
    LspServer server = {0};
    snprintf(server.root_path, sizeof(server.root_path), "%s", root_path);

    FILE *in = fdopen(stdin_fd, "r");
    FILE *out = fdopen(stdout_fd, "w");
    if (!in || !out) return 1;

    bool running = true;
    bool initialized = false;

    while (running) {
        char *content = read_message(in);
        if (!content) break;

        if (!is_request(content)) {
            free(content);
            continue;
        }

        char *method = extract_method(content);
        long id = parse_request_id(content);

        if (!method) {
            free(content);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(out, id);
            initialized = true;
        } else if (strcmp(method, "initialized") == 0) {
            // Server ready
        } else if (strcmp(method, "shutdown") == 0) {
            handle_shutdown(out, id);
            running = false;
        } else if (strcmp(method, "exit") == 0) {
            running = false;
        } else if (initialized) {
            if (strcmp(method, "textDocument/hover") == 0) {
                handle_hover(&server, out, id, content);
            } else if (strcmp(method, "textDocument/definition") == 0) {
                handle_definition(&server, out, id, content);
            } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
                handle_semantic_tokens(&server, out, id, content);
            } else if (strcmp(method, "textDocument/didOpen") == 0) {
                handle_did_open(&server, out, content);
            }
            // Other notifications (didChange, didClose, ...) are ignored:
            // unsupported input fails closed without a response.
        }

        free(method);
        free(content);
    }

    docs_free(&server);
    fclose(in);
    fclose(out);
    return 0;
}
