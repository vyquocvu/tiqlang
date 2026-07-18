# Tiq Implementation Status

Updated: 2026-07-18

## Current milestone

M2 — Static semantics.

## Implemented

- Repository identity, license, and design principles
- Draft Tiq v0.1 language specification
- Lexical and expression grammar
- Type-system and memory-model direction
- C11 bootstrap compiler entry point
- `tiq --version`
- `tiq emit-c <file.tiq>`
- `tiq build <file.tiq> -o <output>`
- POSIX `mkstemp`, `TMPDIR`, `fork`, `execvp`, and `waitpid` build path for temporary C files and host C compiler invocation without shell interpretation
- String print statements: `!"text"`
- Fail-closed rejection of unsupported statements
- Shell smoke test for compile, execute, and reject paths
- Golden diagnostic tests for bootstrap malformed-input boundaries
- Linux and macOS CI workflow running required build, test, and sanitizer checks
- M1.1: Source and lexer (tokens, positions, comments, strings, integers, identifiers, reserved operators).
- M1.3: Diagnostics (stable error codes, source spans, expected/found messages).
- M1.2: Parser and AST (expressions, bindings, conditional expressions, function definitions, calls, and blocks).

## Not implemented

- Escape decoding beyond preserving source spelling
- Type checking, semantic analysis
- `run`, `check`, `fmt`, and `test` CLI commands
- Package system or standard library
- Ownership checking
- Cross-platform non-POSIX process spawning

## Known bootstrap limitations

- Temporary-file creation and host compiler execution currently use POSIX APIs; non-POSIX platforms need an equivalent documented process abstraction.
- The implementation is a proof of the compilation path, not a production compiler.

## Next package

M2: Static semantics (lexical scopes and symbols).
