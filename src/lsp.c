#define _POSIX_C_SOURCE 200809L
#include "../include/lsp.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>

static char lsp_root_path[1024] = {0};

int lsp_server_init(const char *root_path) {
    snprintf(lsp_root_path, sizeof(lsp_root_path), "%s", root_path);
    return 0;
}

void lsp_server_shutdown(void) {
    // Nothing to clean up
}

static char *read_message(FILE *in) {
    // Read Content-Length header
    char header[256];
    size_t header_len = 0;
    int c;

    while ((c = fgetc(in)) != EOF) {
        if (c == '\n') {
            header[header_len] = '\0';
            if (header_len == 0) break; // Empty line = end of headers
            header_len = 0;
            continue;
        }
        if (header_len < sizeof(header) - 1) {
            header[header_len++] = (char)c;
        }
    }

    // Parse Content-Length
    int content_length = 0;
    char *colon = strchr(header, ':');
    if (colon) {
        while (*++colon == ' ');
        content_length = atoi(colon);
    }

    if (content_length <= 0) return NULL;

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

// Helper to build JSON strings without asprintf
static void send_notification(FILE *out, const char *method, const char *params) {
    char content[4096];
    snprintf(content, sizeof(content),
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
             method, params);
    send_message(out, content);
}

static void send_response(FILE *out, long id, const char *result) {
    char content[4096];
    snprintf(content, sizeof(content),
             "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}",
             id, result);
    send_message(out, content);
}

static void publish_diagnostics(FILE *out, const char *uri) {
    char params[512];
    snprintf(params, sizeof(params),
             "{\"uri\":\"%s\",\"diagnostics\":[]}", uri);
    send_notification(out, "textDocument/publishDiagnostics", params);
}

static void handle_initialize(FILE *out, long id) {
    const char *capabilities =
        "{"
        "\"compiler\":{"
        "  \"SemanticTokensProvider\":{\"full\":true},"
        "  \"HoverProvider\":true,"
        "  \"DefinitionProvider\":true"
        "},"
        "\"textDocument\":{"
        "  \"syncKind\":1"
        "}"
        "}";
    send_response(out, id, capabilities);
}

static void handle_initialized(FILE *out) {
    (void)out;
    // Server ready
}

static void handle_shutdown(FILE *out, long id) {
    send_response(out, id, "null");
}

static void handle_text_document_hover(FILE *out, long id) {
    const char *result = "{\"contents\":{\"kind\":\"markdown\",\"value\":\"**Tiq Symbol**\"}}";
    send_response(out, id, result);
}

static void handle_text_document_definition(FILE *out, long id) {
    const char *result = "{\"uri\":\"file:///main.tiq\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":5}}}";
    send_response(out, id, result);
}

static void handle_text_document_semantic_tokens(FILE *out, long id) {
    const char *result = "{\"data\":[0,0,5,0,0]}";
    send_response(out, id, result);
}

static int parse_request_id(const char *content) {
    const char *id_str = strstr(content, "\"id\":");
    if (!id_str) return -1;
    id_str += 5;
    while (*id_str == ' ') id_str++;
    if (*id_str == '"') {
        id_str++;
        return atoi(id_str);
    }
    return atoi(id_str);
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
    lsp_server_init(root_path);

    FILE *in = fdopen(stdin_fd, "r");
    FILE *out = fdopen(stdout_fd, "w");
    if (!in || !out) return 1;

    // Send server info
    const char *server_info =
        "{\"jsonrpc\":\"2.0\",\"result\":{"
        "\"name\":\"tiq\",\"version\":\"0.1.0\""
        "}}";
    send_message(out, server_info);

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
            handle_initialized(out);
        } else if (strcmp(method, "shutdown") == 0) {
            handle_shutdown(out, id);
            running = false;
        } else if (strcmp(method, "exit") == 0) {
            running = false;
        } else if (initialized) {
            if (strcmp(method, "textDocument/hover") == 0) {
                handle_text_document_hover(out, id);
            } else if (strcmp(method, "textDocument/definition") == 0) {
                handle_text_document_definition(out, id);
            } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
                handle_text_document_semantic_tokens(out, id);
            } else if (strcmp(method, "textDocument/didOpen") == 0) {
                // Extract uri from content (simplified)
                const char *text_uri = strstr(content, "\"textDocument\"");
                if (text_uri) {
                    const char *uri_pos = strstr(text_uri, "\"uri\"");
                    if (uri_pos) {
                        const char *colon = strchr(uri_pos, ':');
                        if (colon) {
                            // Skip past "uri":"
                            while (*++colon == ' ' || *colon == '"') {}
                            const char *end = strchr(colon, '"');
                            if (end) {
                                size_t uri_len = (size_t)(end - colon);
                                char uri[512];
                                if (uri_len < sizeof(uri)) {
                                    memcpy(uri, colon, uri_len);
                                    uri[uri_len] = '\0';
                                    // For now, send empty diagnostics
                                    publish_diagnostics(out, uri);
                                }
                            }
                        }
                    }
                }
            }
        }

        free(method);
        free(content);
    }

    fclose(in);
    fclose(out);
    return 0;
}
