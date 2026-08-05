// M17.2: QBE IL emitter — translates the Tiq SSA IR to QBE IL text.
// The output is a complete QBE IL file that can be assembled into a
// native object file by the QBE binary.
#ifndef TIQ_EMIT_QBE_H
#define TIQ_EMIT_QBE_H

#include <stdio.h>
#include "ir.h"

// Emit QBE IL for the given IR module to `out`.
// The main function is exported as $tiq_user_main (called by the C
// runtime's main). User functions are emitted with their original names.
// Returns true on success, false on error.
bool emit_qbe(FILE *out, const IrModule *module);

#endif
