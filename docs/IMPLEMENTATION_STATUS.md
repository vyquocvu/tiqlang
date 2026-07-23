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
- M3: Bracket loop condition type validation — while-style condition must be `bool`, range bounds must be `int`, rejected with source-located diagnostics.
- M3: Array literals (`[1, 2, 3]`) with uniform element type inference.
- M3: Array indexing (`xs[0]`) with int-index validation.
- M3: Array binding emission (`int xs[3] = {1, 2, 3};`).
- M3: Mutable array element assignment (`xs[0] <- 99`, `xs[0] += 1`) with compound operators.
- M3: Print rejects array types (ERR_TYPE_MISMATCH: "cannot print array directly").
- M3: Runtime bounds checking on array access (ternary guard on read, conditional guard on write).
- M3: Identifiers carry full TYPE_ARRAY metadata (element_type, array_length).
- M3: TYPE_ARRAY type name in dump-typed-ast output.
- M3: Memory leak fix — element_type freed in parser_free for TYPE_ARRAY.
- LANGUAGE_SPEC §13 updated from "Planned v0.2" to concrete v0.1 array spec.

## Not implemented

- Escape decoding beyond preserving source spelling
- Slices and slice syntax (`xs[i..j]`) (planned v0.2)
- Bounds checks (depends on slices for variable-range access)
- String views (depends on slices)
- Minimal collection primitives (beyond literal arrays)
- Mutable array element assignment
- Ownership (M4)
- Tooling commands (`run`, `check`, `fmt`, `test`) (M5)
- Package system or standard library (M6)
- Cross-platform non-POSIX process spawning

## Known bootstrap limitations

- Temporary-file creation and host compiler execution currently use POSIX APIs; non-POSIX platforms need an equivalent documented process abstraction.
- The implementation is a proof of the compilation path, not a production compiler.
- Block bodies produce double braces in generated C (cosmetic, valid C11).

## Next package

M3: Slices and bounds checks. Requires LANGUAGE_SPEC slice syntax specification first.
