# Tiq Implementation Status

Updated: 2026-07-18

## Current milestone

M0 — Repository and language baseline.

## Implemented

- Repository identity, license, and design principles
- Draft Tiq v0.1 language specification
- Lexical and expression grammar
- Type-system and memory-model direction
- C11 bootstrap compiler entry point
- `tiq --version`
- `tiq emit-c <file.tiq>`
- `tiq build <file.tiq> -o <output>`
- String print statements: `!"text"`
- Fail-closed rejection of unsupported statements
- Shell smoke test for compile, execute, and reject paths

## Not implemented

- General lexer/token stream
- AST parser
- Expressions, bindings, functions, or types
- Escape decoding beyond preserving source spelling
- `run`, `check`, `fmt`, and `test` CLI commands
- Package system or standard library
- Ownership checking
- CI and cross-platform process spawning

## Known bootstrap limitations

- The compiler currently invokes the host compiler through `system()`.
- Temporary-file creation uses the C bootstrap path and must be replaced with a secure platform abstraction.
- Paths containing hostile shell syntax are not yet supported safely.
- The implementation is a proof of the compilation path, not a production compiler.

## Next package

M0.1: replace temporary/process handling with safe platform functions, add CI, and prove `make test` on Linux and macOS.
