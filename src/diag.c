#include "../include/diag.h"
#include <stdio.h>

void diag_init(DiagContext *diag) {
    diag->has_error = false;
    diag->fatal_error = false;
}

void diag_error(DiagContext *diag, const char *path, int line, ErrorCode code, const char *message) {
    (void)code; // Currently not printing the error code, just the message, to match the golden tests

    if (diag->fatal_error) return;

    fprintf(stderr, "%s:%d: error: %s\n", path, line, message);
    diag->has_error = true;

    // We can define which errors are fatal structural errors to stop cascading
    if (code == ERR_EXPECTED_PRINT || code == ERR_EXPECTED_STRING || code == ERR_UNSUPPORTED_STATEMENT) {
        diag->fatal_error = true;
    }
}
