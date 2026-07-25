#ifndef TIQ_DIAG_H
#define TIQ_DIAG_H

#include <stdbool.h>

typedef enum {
    ERR_UNEXPECTED_CHAR,
    ERR_UNTERMINATED_STRING,
    ERR_NEWLINE_IN_STRING,
    ERR_EXPECTED_PRINT,
    ERR_EXPECTED_STRING,
    ERR_UNEXPECTED_TOKEN,
    ERR_EXPECTED_EXPRESSION,
    ERR_EXPECTED_IDENTIFIER,
    ERR_UNSUPPORTED_STATEMENT,
    ERR_UNDEFINED_SYMBOL,
    ERR_TYPE_MISMATCH,
    ERR_UNSUPPORTED_CONVERSION,
    ERR_IMMUTABLE_ASSIGNMENT,
    ERR_ARITY_MISMATCH,
    ERR_EXPECTED_BLOCK,
    ERR_CONDITION_TYPE,
    ERR_LOOP_VARIABLE,
    ERR_BREAK_OUTSIDE_LOOP,
    ERR_CANNOT_MOVE_IMMUTABLE,
    ERR_USE_AFTER_MOVE
} ErrorCode;

typedef struct {
    bool has_error;
    bool fatal_error;
} DiagContext;

void diag_init(DiagContext *diag);
void diag_error(DiagContext *diag, const char *path, int line, ErrorCode code, const char *message);

#endif
