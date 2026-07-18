# Tiq Roadmap

Status labels: `done`, `active`, `queued`, `blocked`.

## M0 — Repository and language baseline

Status: done

- [x] Project identity and README
- [x] Draft language specification v0.1
- [x] Grammar and operator precedence
- [x] Type-system direction
- [x] Memory-model direction
- [x] Compiler architecture
- [x] C11 bootstrap build
- [x] First native compilation slice: string print statement
- [x] Safe POSIX temporary C file and host compiler invocation without shell interpretation
- [x] CI on Linux and macOS
- [x] Golden diagnostic tests

Exit gate: clean checkout can build `tiq`, compile `examples/hello.tiq`, run it, and reject malformed input.

## M1 — Real frontend

Status: done

### M1.1 Source and lexer

Tokens, positions, comments, strings, integers, identifiers, and all reserved operators.

### M1.2 Parser and AST

Immutable bindings, mutable bindings, reassignment, arithmetic expressions, comparison, boolean logic, conditional expressions, function definitions, calls, and blocks.

### M1.3 Diagnostics

Stable error codes, source spans, expected/found messages, and no cascading diagnostics after a fatal structural error.

Exit gate: parser golden tests cover every grammar production and malformed boundary.

## M2 — Static semantics

Status: queued

- lexical scopes and symbols;
- primitive types;
- local inference;
- explicit conversions;
- mutability checks;
- function type checking;
- deterministic typed IR.

Exit gate: invalid programs are rejected before code generation and typed IR snapshots are stable.

## M3 — Control flow and collections

Status: queued

- while and range loops;
- arrays and slices;
- bounds checks;
- break and continue;
- string views;
- minimal collection primitives.

## M4 — Ownership

Status: queued

- owned values and moves;
- borrows;
- scope destruction;
- allocator interface;
- explicit shared ownership library type.

## M5 — Tooling

Status: queued

- formatter;
- test runner;
- package manifests and deterministic lockfile;
- incremental module cache;
- language server protocol baseline.

## M6 — Service-ready standard library

Status: queued

- filesystem and process APIs;
- sockets and HTTP primitives;
- JSON parser/encoder;
- structured concurrency after ownership is proven;
- cross-compilation matrix.

## Explicitly deferred

- self-hosting;
- macros;
- direct LLVM/native backend;
- garbage-collected mode;
- dynamic linking ABI stability;
- arbitrary operator overloading.
