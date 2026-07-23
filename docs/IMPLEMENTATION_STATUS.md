# Tiq Implementation Status

Updated: 2026-07-23

## Current milestone

M3 — Control flow and collections.

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
- M2: Lexical scopes and symbols.
- M2: Primitive types.
- M2: Local inference.
- M2: Explicit conversions (fail closed).
- M2: Mutability checks.
- M2: Function type checking (arity checking).
- M2: Deterministic typed IR.
- M3: While loops (parse, type-check, C11 emission via `while (cond) { body }`).
- M3: For-in range loops (parse, type-check, C11 emission via `for (int var = start; var < end; var++)`).
- M3: Break and continue (parse, type-check with loop-context validation, C11 emission).
- M3: C expression emitter for all AST node types (arithmetic, logic, comparisons, conditionals, functions, calls, bindings, assignments, blocks).

## Not implemented

- Escape decoding beyond preserving source spelling
- Arrays and slices (blocked: LANGUAGE_SPEC §13 marks as "Planned v0.2"; provisional type syntax)
- Bounds checks (depends on arrays/slices)
- String views (depends on slices)
- Minimal collection primitives (depends on arrays)
- Ownership (M4)
- Tooling commands (`run`, `check`, `fmt`, `test`) (M5)
- Package system or standard library (M6)
- Cross-platform non-POSIX process spawning

## Known bootstrap limitations

- Temporary-file creation and host compiler execution currently use POSIX APIs; non-POSIX platforms need an equivalent documented process abstraction.
- The implementation is a proof of the compilation path, not a production compiler.
- Block bodies produce double braces in generated C (cosmetic, valid C11).

## Next package

M3: Arrays and slices. Blocked until LANGUAGE_SPEC provides complete array/slice type specification (currently "Planned v0.2 syntax" in §13).
