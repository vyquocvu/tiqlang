#include "../include/diag.h"
#include <stdio.h>

void diag_init(DiagContext *diag) {
    diag->has_error = false;
    diag->fatal_error = false;
    diag->records = NULL;
    diag->record_cap = 0;
    diag->record_count = 0;
}

// Fatal structural errors stop cascading: once one is reported, further
// diagnostics are suppressed.
static const ErrorCode FATAL_CODES[] = {
    ERR_UNSUPPORTED_STATEMENT,
};

static bool is_fatal(ErrorCode code) {
    for (size_t i = 0; i < sizeof FATAL_CODES / sizeof FATAL_CODES[0]; i++) {
        if (FATAL_CODES[i] == code) return true;
    }
    return false;
}

void diag_error(DiagContext *diag, const char *path, int line, ErrorCode code, const char *message) {
    if (diag->fatal_error) return;

    if (diag->records) {
        // Structured sink: the consumer owns presentation, so nothing is
        // printed here. Records past the cap are dropped (fail closed).
        if (diag->record_count < diag->record_cap) {
            DiagRecord *r = &diag->records[diag->record_count++];
            r->line = line;
            r->code = code;
            snprintf(r->message, sizeof(r->message), "%s", message);
        }
    } else {
        fprintf(stderr, "%s:%d: error[E%02d]: %s\n", path, line, (int)code, message);
    }
    diag->has_error = true;

    if (is_fatal(code)) {
        diag->fatal_error = true;
    }
}
