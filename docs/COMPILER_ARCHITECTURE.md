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
!"text"
```

It validates source, emits a C program using `fputs`, and optionally invokes the host `cc`. Unsupported syntax fails closed.

## Required invariants

- Diagnostics include source path and line number.
- Invalid input never produces an executable.
- Generated C is deterministic for identical source and compiler version.
- Temporary files are removed on success and failure.
- Shell arguments derived from user paths must be quoted or passed without shell interpretation in a future process abstraction.
- Each milestone adds syntax only after lexer, parser, diagnostics, and tests agree on its behavior.

## Planned modules

```text
src/main.c        CLI orchestration
src/source.c      source loading and positions
src/lexer.c       tokenization
src/parser.c      AST construction
src/resolve.c     scopes and symbols
src/type.c        inference and checking
src/ir.c          typed lower-level representation
src/emit_c.c      portable C backend
src/diag.c        structured diagnostics
```

The bootstrap starts in one file to keep the first slice auditable. It must be split by milestone M1.2 before expression parsing expands.

## Backend strategy

C11 remains the reference backend through v0.1. A direct native or LLVM backend is explicitly out of scope until semantics, ABI, and tests are stable.
