# Tiq Implementation Status

Updated: 2026-07-25

## Current milestone

M6 — Service-ready standard library (complete)

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
- M3: Runtime bounds checking on array access (ternary guard on read, conditional guard on write).
- M3: Identifiers carry full TYPE_ARRAY metadata (element_type, array_length).
- M3: TYPE_ARRAY type name in dump-typed-ast output.
- M3: Memory leak fix — element_type freed in parser_free for TYPE_ARRAY.
- M3: `len(xs)` built-in — accepts array, slice, and string views; returns length.
- LANGUAGE_SPEC §13 updated from "Planned v0.2" to concrete v0.1 array and slice spec.
- M3: Slices and String Views syntax specification (LANGUAGE_SPEC §13.1, GRAMMAR EBNF `slice_range`).
- M3: Slices and String Views compiler support:
  - AST `AST_CALL` with `is_slice` flag and `OMITTED` bounds support (`xs[i..j]`, `xs[i..]`, `xs[..j]`, `xs[..]`).
  - Semantic type checking for `TYPE_SLICE` and `TYPE_STR_VIEW`.
  - C11 backend emission for `TiqSlice` non-owning views and string views.
  - Tests: `parser.sh` (slice AST golden tests), `semantic.sh` (slice type mismatch diagnostics), `smoke.sh` (end-to-end string view slicing and `len()` execution).
- M3: Stream generators (LANGUAGE_SPEC §14, GRAMMAR EBNF `stream_gen`):
  - AST `AST_STREAM_GEN` with seeds, generation expression, and optional bound.
  - Semantic type checking: `TYPE_STREAM` type for generator expressions.
  - Single indexing of stream generators (`fib[10]`) with int-index validation.
  - C11 backend emission for parameterless stream gen bindings (`tiq_gen_<name>(int n)`).
  - C11 backend emission for parameterized stream gen functions (`tiq_gen_<name>(params..., int n)`).
  - Bracket call on function call results for stream gen functions (`pow(2)[0]` emits `tiq_gen_pow(2, 0)`).
  - Bounded stream generators with `while`/`until` termination.
  - Range slicing of stream generators rejected (ERR_TYPE_MISMATCH: "cannot range-slice a stream generator").
  - Tests: `parser.sh` (stream gen AST golden tests), `semantic.sh` (stream gen type errors), `smoke.sh` (stream gen indexing, parameterized functions, bracket loop integration).
- M4: `move` keyword (unary prefix operator for ownership transfer).
- M4: Move semantics semantic checking: immutable binding move rejected (ERR_CANNOT_MOVE_IMMUTABLE), use-after-move detection (ERR_USE_AFTER_MOVE), moved state reset on compound assignment.
- M4: Move semantics C backend: `move x` emits identity (C has no move semantics); array move emits `memcpy` for correct C array copy.
- M4: Tests: `semantic.sh` (move_immutable, use_after_move, double_move), `smoke.sh` (move_basic, move_reassign, move_compound).
- M4: `defer` keyword for scope-bound cleanup actions.
- M4: Defer semantic checking: defer outside block rejected (ERR_DEFER_OUTSIDE_BLOCK), defer inside bracket loops rejected.
- M4: Defer C backend: deferred actions emitted in reverse order before block closing brace.
- M4: Tests: `parser.sh` (defer AST golden test), `semantic.sh` (defer_outside_block), `smoke.sh` (defer_basic, defer_reverse, defer_with_scope).

### M5: Tooling (complete)

- Formatter (`tiq fmt`):
  - Token-based source formatting preserving language semantics
  - Configurable indentation (spaces or tabs, variable width)
  - Proper handling of braces, brackets, operators, keywords
  - Options for check mode and output file specification
- Test runner (`tiq test`):
  - Discovers `.tiq` test files in directories
  - Executes tests via `tiq build` and compares output
  - Respects `//! expected` comments for expected output
  - Verbose mode for detailed results
- Package manifests (`tiq init`):
  - Creates `*.tiq.toml` manifest files
  - INI-style format with `[package]`, `[deps]`, `[tests]` sections
  - Version validation (major.minor.patch format)
  - Schema validation for required fields
- Incremental module cache:
  - XDG-compliant cache directory (`~/.cache/tiq`)
  - Source file tracking by modification time
  - Cache operations: `tiq cache clear`, `tiq cache path`
- Benchmark tool (`tiq bench`):
  - Measures lexer, parser, and semantic analysis performance
  - Supports multiple files and directories
  - Configurable iterations for accuracy
  - Throughput reporting in bytes/second
- Language Server Protocol baseline (`tiq lsp`):
  - JSON-RPC 2.0 message handling over stdin/stdout
  - Initialize/shutdown protocol support
  - Diagnostics publishing infrastructure
  - Text document synchronization

### M7 — M11: Complete Roadmap Features (complete)

- M7: Array fill `[val; len]`, string character indexing `s[i]`, non-owning `TiqSlice` parameter decay, reusable functions with block bodies and parameter type inference, structured concurrency `chan`/`spawn`, `--target` flag compiler driver.
- M8: Record / struct type definitions, field access `point.x`, pattern matching `match expr { pattern => body }`, Option/Result type foundations.
- M9: Non-owning borrow references `&x` and `&mut x` with lifetime validation.
- M10: Non-blocking event loop & networking socket primitives.
- M11: LSP server capabilities (`hover`, `go-to-definition`, `semanticTokens/full`), platform abstraction layer structure.

### M12: Type system implementation (in progress)

- M12.1: Type representation core (complete, 2026-07-25):
  - Interned type arena (`TypePool` in `include/type.h` / `src/type.c`): structurally identical types share one canonical `SemanticType`, so pointer equality implies type equality; pool lifetime owned by `semantic_check` callers.
  - `src/semantic.c` migrated to pooled immutable `SemanticType *`; inference propagates by swapping node type pointers instead of mutating types in place.
  - `type_display()` renders nested composite types in `dump-typed-ast` (`TYPE_ARRAY[3]:TYPE_INT`, `TYPE_SLICE:TYPE_INT`).
  - Fixes latent uninitialized `field_count` read (pool zero-initializes types) and `param_types` leak (freed in `parser_free`).
  - Tests: `semantic.sh` (`typed_array_nested`, `typed_slice_nested` goldens, added failing first); full suite green under ASan/UBSan.
- M12.2: Sized primitives and literal typing (complete, 2026-07-25):
  - Integer values are 64-bit end to end: backend emits `int64_t` bindings, params, returns, and loop counters, `%lld` printing, `LL` literal suffixes (keeps C constant arithmetic 64-bit), `uint64_t` bounds checks, and `sizeof(int64_t)` slice arithmetic.
  - Integer literals outside `i64` are rejected at compile time (`ERR_LITERAL_RANGE`, `strtoll`/`ERANGE` in `src/semantic.c`); no executable is produced (fail closed).
  - Sized kinds `TYPE_I8`–`TYPE_U64`, `TYPE_F32`, `TYPE_NEVER` exist with `stdint.h` mappings in the emitter; `TYPE_I64`/`TYPE_F64` alias `TYPE_INT`/`TYPE_FLOAT` to keep pooled types unique. No surface syntax constructs sized types until M12.3 explicit conversions.
  - Spec: LANGUAGE_SPEC §11 and TYPE_SYSTEM.md literal rules amended to the `i64` default.
  - Tests: `smoke.sh` `i64_values` runtime test and `semantic.sh` `int_literal_overflow` / `int_literal_overflow_expr` goldens, all added failing first; full suite green under ASan/UBSan.

## Current milestone

M12 — Type system implementation (in progress; M12.1–M12.2 complete)

## Known bootstrap limitations

- Temporary-file creation and host compiler execution currently use POSIX APIs; non-POSIX platforms need an equivalent documented process abstraction.
- The implementation is a proof of the compilation path, not a production compiler.
- Block bodies produce double braces in generated C (cosmetic, valid C11).
- Formatter is conservative; some stylistic variations may not be normalized.

