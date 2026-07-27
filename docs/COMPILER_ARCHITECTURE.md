# Tiq Compiler Architecture

The bootstrap compiler is written in ISO C11 and initially emits C11.

## Pipeline

```text
source bytes
  -> lexer
  -> parser
  -> AST
  -> name resolution
  -> type checking
  -> typed IR
  -> C11 emitter
  -> host C compiler
  -> native executable
```

## Current vertical slice

The first implementation intentionally collapses the pipeline for one construct:

```tiq
print("text")
```

It validates source, emits a C program using `fputs`, and optionally invokes the host `cc`. Unsupported syntax fails closed.

## Required invariants

- Diagnostics include source path and line number.
- Invalid input never produces an executable.
- Generated C is deterministic for identical source and compiler version.
- Temporary files are removed on success and failure.
- Shell arguments derived from user paths must be quoted or passed without shell interpretation in a future process abstraction.
- Each milestone adds syntax only after lexer, parser, diagnostics, and tests agree on its behavior.

## Modules (actual, 2026-07-27)

```text
src/main.c        CLI orchestration and build driver
src/lexer.c       tokenization
src/parser.c      AST construction
src/semantic.c    scopes, symbols, and type checking
src/type.c        type pool (interned primitive and struct types)
src/emit_c.c      portable C backend (EmitContext, re-entrant)
src/diag.c        structured diagnostics
src/formatter.c   canonical source formatter
src/cache.c       build cache
src/tester.c      test runner
src/manifest.c    project manifest
src/lsp.c         language server scaffold
src/benchmark.c   compile benchmark harness
```

There is no separate `src/source.c`, `src/resolve.c`, or `src/ir.c`: source loading lives in `main.c`, resolution and checking are combined in `semantic.c`, and the emitter walks the typed AST directly without a lower-level IR. The C backend was split out of `main.c` into `src/emit_c.c` in 2026-07-27; all emitter state is carried in an `EmitContext` (no mutable globals), so `compile_to_c` is re-entrant and unit-tested in `tests/unit/test_main.c`.

## Backend strategy

C11 remains the reference backend through v0.1. A direct native or LLVM backend is explicitly out of scope until semantics, ABI, and tests are stable.
