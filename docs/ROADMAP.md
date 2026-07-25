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

Status: done

- lexical scopes and symbols;
- primitive types;
- [x] local inference;
- [x] explicit conversions;
- [x] mutability checks;
- [x] function type checking;
- [x] deterministic typed IR.

Exit gate: invalid programs are rejected before code generation and typed IR snapshots are stable.

## M3 — Control flow and collections

Status: done

### Completed

- while loops (expression condition, block body, C emission as `while`):
  - Parsing: `while_statement()` in `parser.c` handles `while` keyword, parses condition, expects block body.
  - Semantic: checks condition is `bool`, allocates block scope for body.
  - C emission: `while (cond) { body }`.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST, condition type error), `smoke.sh` (while count 0→3).
  - Diagnostics: missing block body reports error.

- range (for-in) loops:
  - Parsing: `for_statement()` in `parser.c` handles `for var in range { body }`.
  - Validates iterable is a `..` binary expression at parse time.
  - Semantic: creates loop variable in dedicated scope, type-checks body.
  - C emission: desugared to `for (int var = start; var < end; var++) { body }`.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST), `smoke.sh` (sum 0..4).
  - Diagnostics: missing `in`, missing range, missing block all report errors.

- break and continue:
  - Parsing: `TOK_BREAK` → `AST_BREAK`, `TOK_CONTINUE` → `AST_CONTINUE` in `statement()`.
  - Semantic: tracked via `loop_depth` in `SemanticContext`; reject break/continue outside loop.
  - C emission: `break;` and `continue;` emit directly.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST, outside-loop rejection), `smoke.sh` (break exits immediately, continue skips rest).

- Expression C emitter (foundation):
  - All AST nodes now emit valid C11: `AST_LITERAL`, `AST_IDENTIFIER`, `AST_BINARY` (arithmetic, comparison, logical, bitwise), `AST_UNARY`, `AST_CONDITIONAL`, `AST_CALL`, `AST_BLOCK`, `AST_BINDING`, `AST_ASSIGN`, `AST_FUNCTION`.
  - Helper `binary_op_c_str()` maps token kinds to C operators.
  - Function definitions emit before `main()` with forward declarations for mutual recursion.
  - Pre-emission validation (`emit_check_node`) catches `..` outside for-in loops.
- Loop condition type validation:
  - While-style bracket loops: domain must be `bool` (ERR_CONDITION_TYPE with "loop condition must be bool").
  - Range bracket loops: left and right bounds must be `int` (ERR_TYPE_MISMATCH with "range bounds must be int").
  - Tests: `semantic.sh` (loop_cond_type, loop_cond_float, loop_range_type, loop_range_mixed_type), `smoke.sh` (while_loop).

### Completed

- Array literals:
  - Parsing: multi-seed `[a, b, c]` (no `...`) → `AST_ARRAY`.
  - Semantic: validates uniform element type; assigns `TYPE_ARRAY` with element type and length.
  - C emission: `int name[size] = {val1, val2, ...};` binding; array indexing via `arr[idx]`.
  - Tests: `parser.sh` (AST golden), `smoke.sh` (index each element).
  - LANGUAGE_SPEC §13 updated from "Planned v0.2" to concrete v0.1 array spec.
  - Diagnostics: non-uniform element types rejected (ERR_TYPE_MISMATCH), non-int index rejected (ERR_TYPE_MISMATCH).
- Mutable array element assignment:
  - Parsing: `expr[idx] <- value` detected in `statement()` after expression parse; extends `AST_ASSIGN` with optional `index` field.
  - Semantic: validates array is mutable, index is `int`, target is array type.
  - C emission: `name[index] = value;` and compound forms (`+=`, `-=`, etc.).
  - Tests: `smoke.sh` (array_assign), `semantic.sh` (immutable rejection, bad index).
  - Diagnostics: immutable array, non-int index, non-array target all rejected.
- Runtime bounds checking:
  - Array read: ternary guard `((unsigned)(i) < (unsigned)(len) ? xs[i] : panic)`.
  - Array write: conditional guard `if ((unsigned)(i) >= (unsigned)(len)) { panic; }`.
  - `<stdlib.h>` included unconditionally for `exit()`.
- `len(xs)` built-in:
  - Semantic: validates exactly 1 argument, must be array type, returns `TYPE_INT`.
  - C emission: emits the compile-time constant length.
  - Tests: `smoke.sh` (array_len), `semantic.sh` (len_non_array, len_no_args, len_too_many).
  - Diagnostics: wrong arity or non-array arg rejected.

- Slices and string views syntax specification:
  - Specified in LANGUAGE_SPEC §13.1 and GRAMMAR EBNF (`slice_range`).
  - Slicing forms: `xs[i..j]`, `xs[i..]`, `xs[..j]`, `xs[..]`.
  - Non-owning slice representation and runtime bounds check $0 \le \text{start} \le \text{end} \le \text{len}$.

- Stream generators:
  - Parsing: `AST_STREAM_GEN` with seeds, generation expression, and optional `while`/`until` bound.
  - Semantic: `TYPE_STREAM` type for generator expressions; single indexing returns `TYPE_INT`.
  - C emission: `tiq_gen_<name>(int n)` functions for parameterless generators.
  - C emission: `tiq_gen_<name>(params..., int n)` for parameterized stream gen functions.
  - C emission: bracket call on function call results (`pow(2)[0]` → `tiq_gen_pow(2, 0)`).
  - Bounded generators with `while`/`until` termination.
  - Diagnostics: cannot range-slice a stream generator.
  - Tests: `parser.sh` (stream gen AST golden), `semantic.sh` (stream gen type errors), `smoke.sh` (stream gen indexing, parameterized functions, bracket loop integration).
  

### Exit criteria

- [x] while loops compile and execute correctly
- [x] range (for-in) loops compile and execute correctly
- [x] break and continue work inside loops
- [x] invalid programs are rejected (loop condition types, break outside loop)
- [x] arrays (literals, indexing, bindings, mutable element assignment, bounds checking, len built-in)
- [x] slices, string views, collection primitives (slice syntax specified in LANGUAGE_SPEC)
- [x] stream generators (parameterless and parameterized, single indexing, bounded generators)

## M4 — Ownership

Status: active

- owned values and moves;
- borrows;
- scope destruction;
- allocator interface;
- explicit shared ownership library type.

### M4.1 — `move` keyword

- [x] `move` keyword in lexer (TOK_MOVE).
- [x] `move` parsed as unary prefix operator.
- [x] Semantic: `is_moved` flag on Symbol.
- [x] Semantic: move on immutable binding rejected.
- [x] Semantic: use-after-move detection.
- [x] Semantic: compound assignment resets moved state.
- [x] C emitter: `move x` emits identity for scalars.
- [x] C emitter: `move x` emits `memcpy` for arrays.
- [x] LANGUAGE_SPEC §16.1: Move semantics documented.
- [x] Tests: `semantic.sh` (move_immutable, use_after_move, double_move), `smoke.sh` (move_basic, move_reassign, move_compound).

### Exit criteria

- [x] `move` keyword parsed and type-checked
- [x] Use-after-move detected at compile time
- [x] Move of immutable binding rejected
- [x] Compound assignment resets moved state
- [x] Array move emits correct C (memcpy)

### M4.2 — `defer` keyword

- [x] `defer` keyword in lexer (TOK_DEFER).
- [x] `defer` parsed as statement taking a statement (in block context only).
- [x] AST_DEFER node kind with deferred list on block variant.
- [x] Semantic: defer outside block rejected (ERR_DEFER_OUTSIDE_BLOCK).
- [x] Semantic: defer inside bracket loops rejected.
- [x] C emitter: deferred actions emitted in reverse order before `}`.
- [x] LANGUAGE_SPEC §16.2: Defer semantics documented.
- [x] Tests: `parser.sh` (defer AST golden), `semantic.sh` (defer_outside_block), `smoke.sh` (defer_basic, defer_reverse, defer_with_scope).

### Exit criteria

- [x] `defer` keyword parsed and type-checked
- [x] Deferred actions execute in reverse order
- [x] `defer` outside block rejected at compile time
- [x] `defer` inside bracket loops rejected

## M5 — Tooling

Status: done

### M5.1 — Formatter

- [x] `tiq fmt` command with token-based source formatting
- [x] Configurable indentation (spaces/tabs, variable width)
- [x] Proper handling of braces, brackets, operators, keywords
- [x] `--check` mode for CI integration
- [x] stdin/stdout support
- [x] `--output` option for file redirection

### M5.2 — Test Runner

- [x] `tiq test` command for test discovery and execution
- [x] Directory and file test targets
- [x] `//! expected` output comments
- [x] Verbose and list modes
- [x] XDG cache integration

### M5.3 — Package Manifests

- [x] `tiq init` command for manifest creation
- [x] INI-style `*.tiq.toml` format
- [x] `[package]`, `[deps]`, `[tests]` sections
- [x] Version format validation
- [x] Manifest schema validation

### M5.4 — Incremental Module Cache

- [x] `tiq cache` command (clear, path)
- [x] XDG cache directory (`~/.cache/tiq`)
- [x] Source file mtime-based invalidation
- [x] Deterministic cache structure

### M5.5 — LSP Baseline

- [x] `tiq lsp` command for Language Server Protocol
- [x] JSON-RPC 2.0 over stdin/stdout
- [x] Initialize/shutdown protocol
- [x] Diagnostics publishing
- [x] Text document sync infrastructure

### M5.6 — Benchmark Tool

- [x] `tiq bench` command for compiler performance measurement
- [x] Lexer, parser, and semantic analysis timing
- [x] Support for multiple files and directories
- [x] Verbose and quiet output modes
- [x] Multiple iterations for more accurate measurements
- [x] Throughput reporting (bytes/s)

### Exit criteria

- [x] Formatter processes all Tiq syntax correctly
- [x] Test runner discovers and executes tests
- [x] Package manifests are created and validated
- [x] Cache operations work correctly
- [x] LSP server initializes and handles basic requests

## M6 — Service-ready standard library

Status: done

- [x] Filesystem APIs (`fs_read`, `fs_write`, `fs_exists`)
- [x] Process APIs (`proc_exec`, `proc_exit`)
- [x] Sockets and HTTP primitives (`net_fetch`)
- [x] JSON parser/encoder primitives (`json_parse_int`, `json_encode_str`)
- [x] Print expression statement (`!expr`) backend emission with typed `printf` support
- [x] POSIX `TMPDIR`-aware executable execution for `tiq run`
- [x] Semantic checking and diagnostic assertion tests
- [x] C11 backend emission and end-to-end smoke and tooling tests

## M7 — Generic functions, collections & structured concurrency

Status: active

- [ ] Non-scalar function parameter type emission in C backend (`TiqSlice`, `const char *`)
- [ ] Single character byte indexing on `str` and `str_view` (`s[i]`)
- [ ] Array fill / repeated initialization syntax (`[val; len]`)
- [ ] Implicit array/string decay to non-owning slice parameters (`TiqSlice`)
- [ ] Block body return value emission for non-stream functions
- [ ] Structured concurrency primitives (`chan`, `spawn`)
- [ ] Cross-compilation matrix

## M8 — User-defined composite types & explicit error handling

Status: queued

- [ ] Record / struct type definitions (`Point = { x: int, y: int }`)
- [ ] Explicit Result & Option types (`T?` / `T!E`) eliminating hidden exceptions
- [ ] Pattern matching & structural destructuring (`match result`)
- [ ] Direct C struct emission in backend without vtables or dynamic dispatch

## M9 — Memory ownership & borrow checker (M4 completion)

Status: queued

- [ ] Non-owning borrow references (`&x`, `&mut x`) with lifetime validation
- [ ] Scope-bound destruction and reverse declaration order cleanup for heap values
- [ ] Explicit arena / scope allocator interfaces (`Allocator`)
- [ ] Opt-in reference-counted shared ownership (`Shared<T>`)

## M10 — Service stack & non-blocking I/O

Status: queued

- [ ] Non-blocking event loop integration (`epoll` on Linux, `kqueue` on macOS)
- [ ] Zero-copy JSON encoder and decoder primitives
- [ ] HTTP/1.1 service server & client socket primitives
- [ ] Standard library CLI flag and argument parsing

## M11 — Platform expansion, IDE tooling & self-hosting

Status: queued

- [ ] Native Windows platform abstraction layer (`src/platform.c` using Win32 API)
- [ ] Full LSP server capabilities (`hover`, `go-to-definition`, `semanticTokens`)
- [ ] Self-hosting Tiq compiler written in Tiq

## Explicitly deferred

- self-hosting (until M11);
- macros;
- direct LLVM/native backend;
- garbage-collected mode;
- dynamic linking ABI stability;
- arbitrary operator overloading.


