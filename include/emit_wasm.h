// M17.4.2: wasm32-wasi backend — emits a WebAssembly (MVP) binary module
// from the Tiq SSA IR.
#ifndef TIQ_EMIT_WASM_H
#define TIQ_EMIT_WASM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ir.h"

// Emit a complete wasm32 (WASI, "wasi_snapshot_preview1") module for the
// IR module. On success returns a malloc'd buffer in *out (caller frees)
// with *out_len bytes. On failure returns false and writes a NUL-terminated
// diagnostic to err (errlen bytes; pass a buffer of at least 128 bytes).
//
// The module imports fd_write/proc_exit from wasi_snapshot_preview1 and
// exports `_start` (which runs the Tiq main function), `main`, and
// `memory`. String literals live in the data segment; print is served by
// in-module WASI helpers (strlen, print_str, print_i64, print_bool,
// print_f64). Unsupported IR constructs fail closed with a diagnostic.
bool emit_wasm(const IrModule *module, uint8_t **out, size_t *out_len,
               char *err, size_t errlen);

#endif