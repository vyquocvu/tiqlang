#ifndef TIQ_DIAG_H
#define TIQ_DIAG_H

#include <stdbool.h>

// Stable, printed error codes (plan 2.4). Values are part of the observable
// diagnostic format (`error[E0x]:`), so they are pinned explicitly and must
// never be renumbered; retire a code by removing it, not by reusing its value.
typedef enum {
    ERR_UNEXPECTED_CHAR = 1,
    ERR_UNTERMINATED_STRING = 2,
    ERR_NEWLINE_IN_STRING = 3,
    ERR_UNEXPECTED_TOKEN = 4,
    ERR_EXPECTED_EXPRESSION = 5,
    ERR_EXPECTED_IDENTIFIER = 6,
    ERR_UNSUPPORTED_STATEMENT = 7,
    ERR_UNDEFINED_SYMBOL = 8,
    ERR_TYPE_MISMATCH = 9,
    ERR_UNSUPPORTED_CONVERSION = 10,
    ERR_IMMUTABLE_ASSIGNMENT = 11,
    ERR_ARITY_MISMATCH = 12,
    ERR_EXPECTED_BLOCK = 13,
    ERR_CONDITION_TYPE = 14,
    ERR_LOOP_VARIABLE = 15,
    ERR_BREAK_OUTSIDE_LOOP = 16,
    ERR_CANNOT_MOVE_IMMUTABLE = 17,
    ERR_USE_AFTER_MOVE = 18,
    ERR_DEFER_OUTSIDE_BLOCK = 19,
    ERR_LITERAL_RANGE = 20,
    ERR_EMPTY_ARRAY = 21,
    ERR_TYPE_ANNOTATION = 22,  // M12.7.2.D: param:type annotation not supported in v0.1
    ERR_BORROW = 23,           // M9.1: borrow rule violations (LANGUAGE_SPEC §16.3)
    ERR_DUPLICATE_ENUM = 24,   // M13.1-P2: duplicate enum name / enum-struct name collision (§17.5)
    ERR_DUPLICATE_VARIANT = 25,// M13.1-P2: duplicate variant name within one enum (§17.5)
    ERR_UNKNOWN_VARIANT = 26,  // M13.1-P2: Name.X where X is not a variant of enum Name (§17.5)
    ERR_MODULE_NOT_FOUND = 27, // M13.1-P6: import of a missing/unreadable file (§17.6)
    ERR_CIRCULAR_IMPORT = 28   // M13.1-P6: cyclic import chain (§17.6)
} ErrorCode;
// ERR_EXPECTED_PRINT/ERR_EXPECTED_STRING (dead since the print statement was
// removed) were retired before this numbering was first pinned and printed,
// so no published value was ever reused.

// One captured diagnostic for the optional structured sink (M11.1).
typedef struct {
    int line;
    ErrorCode code;
    char message[200];
} DiagRecord;

typedef struct {
    bool has_error;
    bool fatal_error;
    // Optional structured sink: when `records` is non-NULL, diagnostics are
    // captured there instead of printed to stderr (the consumer owns
    // presentation). Records past `record_cap` are dropped; `record_count`
    // counts stored records only.
    DiagRecord *records;
    int record_cap;
    int record_count;
} DiagContext;

void diag_init(DiagContext *diag);
void diag_error(DiagContext *diag, const char *path, int line, ErrorCode code, const char *message);

#endif
