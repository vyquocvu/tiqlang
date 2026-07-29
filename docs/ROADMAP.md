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

Status audit (2026-07-27): error codes are pinned (`E01`-`E20`, `diag.h`) and printed as `path:line: error[E0x]: message` since plan 2.4; the dead `ERR_EXPECTED_PRINT`/`ERR_EXPECTED_STRING` codes were retired before the numbering was first published. Type mismatches print `expected <T>, found <U>` via `type_display()` since plan 3.1 (same day).

Exit gate: parser golden tests cover every grammar production and malformed boundary.

## M2 — Static semantics

Status: done

- lexical scopes and symbols;
- primitive types (coarse `int`/`float`/`str`/`bool` buckets only; sized types move to M12);
- [x] local inference (ad hoc, per-site; unification moves to M12);
- [x] explicit conversions (fail-closed rejection only; real checked conversions move to M12);
- [x] mutability checks;
- [x] function type checking (arity only; signature checking moves to M12);
- [x] deterministic typed IR.

Exit gate: invalid programs are rejected before code generation and typed IR snapshots are stable.

## M3 — Control flow and collections

Status: done

### Completed

- while-style loops (bracket loop with bool condition, C emission as `while`):
  - Parsing: `bracket_loop()` in `parser.c` handles `[cond] { body }`.
  - Semantic: checks condition is `bool`, allocates block scope for body.
  - C emission: `while (cond) { body }`.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST, condition type error), `smoke.sh` (while count 0→3).
  - Diagnostics: missing loop body reports error.

- range loops:
  - Parsing: `bracket_loop()` in `parser.c` handles `[start..end] { body }`.
  - Validates the domain is a `..` binary expression during semantic analysis.
  - Semantic: creates loop variable `i` in dedicated scope, type-checks body, requires int bounds.
  - C emission: desugared to `for (int64_t i = start; i < end; i++) { body }`.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST), `smoke.sh` (sum 0..4).
  - Diagnostics: non-int range bounds and malformed loop bodies report errors.

- break and skip:
  - Parsing: `TOK_BREAK` → `AST_BREAK`, `TOK_SKIP` → `AST_SKIP` in bracket loop bodies.
  - Semantic: tracked via `loop_depth` in `SemanticContext`; reject break/skip outside loop.
  - C emission: `break;` and `continue;` emit directly.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST, outside-loop rejection), `smoke.sh` (break exits immediately, skip skips rest).

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

Status: done (M4.1 `move`, M4.2 `defer`; remaining ownership work in M9)

- owned values and moves; ✅ M4.1
- borrows; → M9
- scope destruction; → M9
- allocator interface; → M9
- explicit shared ownership library type. → M9

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

Status audit 2026-07-25: previously marked done; corrected after source review. Unchecked items below have no working implementation in `src/`.

- [x] Non-scalar function parameter type emission in C backend (`TiqSlice`, `const char *`)
- [x] Single character byte indexing on `str` and `str_view` (`s[i]`)
- [x] Array fill / repeated initialization syntax (`[val; len]`)
- [x] Implicit array/string decay to non-owning slice parameters (`TiqSlice`)
- [x] Block body return value emission for non-stream functions
- [ ] Structured concurrency primitives (`chan`, `spawn`) — parsed only; semantic analysis rejects them fail-closed ("spawn/chan is not supported yet", tested in `tests/semantic.sh`) until a thread/channel runtime exists. The former placeholder emission (`/* spawn thread */ 0`) was removed 2026-07-27.
- [ ] WebAssembly / WASI compilation target support (`tiq build --target wasm32-wasi`) — no `--target` flag exists in the CLI
- [ ] WebAssembly JS host bindings generator (`--target wasm32-unknown-unknown`) — not implemented
- [ ] Cross-compilation matrix — not implemented

## M8 — User-defined composite types & explicit error handling

Status: complete

Status audit 2026-07-25: previously marked done; corrected after source review.
Status audit 2026-07-27 (plan 3.2): struct rows corrected again — no struct/record grammar or parse path exists.
Status audit 2026-07-29: struct definitions, record literals, field access, Option/Result types, and propagation operator implemented.

- [x] Record / struct type definitions and field access — done in M12.6 (2026-07-29)
- [x] Explicit Option types (`T?`) — `some(x)`, `none`, and `??` fallback operator implemented (2026-07-29)
- [x] Explicit Result types (`T!E`) — `ok(x)`, `err(e)`, and `??` fallback operator implemented (2026-07-29)
- [x] Propagation operator (`expr?`) — postfix unwrap for Option/Result, parser lookahead distinguishes from ternary (2026-07-29)
- [x] Pattern matching (`match expr { pattern => body }`) — parsed and checked; arm types unified via `unify()` since plan 3.1 (2026-07-27)
- [x] Direct C struct emission in backend — done in M12.6 (2026-07-29)

## M9 — Memory ownership & borrow checker (M4 completion)

Status: active

Status audit 2026-07-25: previously marked done; corrected after source review.

- [x] Borrow reference syntax (`&x`, `&mut x`) parsed into the AST — since 2026-07-27 rejected during semantic analysis, since 2026-07-29 legal exactly in call argument position for reference parameters (M9.1); everywhere else still fails closed ("borrow is only valid as an argument to a reference parameter", tested in `tests/semantic.sh` / `tests/smoke.sh`)
- [x] M9.1 Borrowed parameters (2026-07-29): `&T` / `&mut T` parameter annotations, `&x` / `&mut x` call arguments, auto-deref in callee bodies, mutation through `&mut` — LANGUAGE_SPEC §16.3, E23 diagnostics. Evidence: `param_ref_kinds` in `src/parser.c`, function registry + borrow checks in `src/semantic.c`, `const T *`/`T *` emission in `src/emit_c.c`; goldens `typed_borrow_param` + 9 negative cases in `tests/semantic.sh`, runtime `m9_borrow_params`/`m9_borrow_shared` in `tests/smoke.sh`
- [x] Borrow lifetime validation for the M9.1 slice — structural: borrows exist only for the duration of one call and cannot be stored, returned, or re-borrowed, so no borrow can outlive its referent; per-call aliasing enforced (many shared, at most one `&mut`, never mixed). General lifetime analysis for stored borrows remains open with the features that would need it
- [ ] Scope-bound destruction and reverse declaration order cleanup for heap values — no destructor emission; only `defer` exists
- [ ] Explicit arena / scope allocator interfaces (`Allocator`) — not present in source
- [ ] Opt-in reference-counted shared ownership (`Shared<T>`) — not present in source

## M10 — Service stack & non-blocking I/O

Status: active

Status audit 2026-07-25: previously marked done; corrected after source review. `net_fetch` is a hardcoded stub returning a fixed JSON string; no sockets, event loop, or HTTP code exists.

- [ ] Non-blocking event loop integration (`epoll` on Linux, `kqueue` on macOS)
- [ ] Zero-copy JSON encoder and decoder primitives (current `json_parse_int`/`json_encode_str` are minimal `atoi`/escape helpers)
- [ ] HTTP/1.1 service server & client socket primitives (replace the `tiq_net_fetch` stub)
- [x] Standard library CLI argument access — M10.1 (2026-07-29): `cli_arg_count()` / `cli_arg(i)` builtins backed by real `argc`/`argv` (LANGUAGE_SPEC §18.1); E12/E09 diagnostics; evidence: `typed_cli_builtins`, `cli_arg_count_bad_arity`, `cli_arg_no_args`, `cli_arg_bad_type` in `tests/semantic.sh`; `m10_cli_args`, `m10_cli_none` runtime tests in `tests/smoke.sh`. Flag parsing (named options) remains open.

## M11 — Platform expansion, IDE tooling & self-hosting

Status: queued

Status audit 2026-07-25: previously marked done; corrected after source review. `src/platform.c` does not exist; there is no wasm playground or self-hosted compiler. Update 2026-07-27 (plan 5.1): LSP `hover`/`definition`/`semanticTokens` now answer with real symbol data from the lexer+parser+semantic front end, pinned by the golden JSON-RPC transcript `tests/tooling/lsp.sh`.

- [ ] Native Windows platform abstraction layer (`src/platform.c` using Win32 API)
- [ ] WebAssembly-compiled in-browser Tiq compiler & interactive web playground
- [x] Full LSP server capabilities (`hover`, `go-to-definition`, `semanticTokens` with real symbol data) — 2026-07-27, evidence: `src/lsp.c` runs the front end on stored `didOpen` text; golden transcript `tests/tooling/lsp.sh`
- [ ] Self-hosting Tiq compiler written in Tiq

## M12 — Type system implementation

Status: in progress (M12.1–M12.5, M12.7 complete; M12.4 done)

Implements `TYPE_SYSTEM.md` as written; prerequisite for M8 Option/Result, M9 ownership checks, and honest function signatures. Each phase lands test-first per `AGENTS.md`.

### M12.1 — Type representation core

- [x] Interned type arena (`TypePool`): structurally identical types canonicalized, pointer equality = type equality
- [x] Replace `SemanticType` value copies and fixed-size field arrays with pooled `Type *`
- [x] `type_display()` for diagnostics and `dump-typed-ast`
- [x] No behavior change: existing golden tests stay green; sanitizer run required

Evidence 2026-07-25: `include/type.h` + `src/type.c` (interned `TypePool`, pool owned by
`semantic_check` callers); `src/semantic.c` migrated to pooled immutable `SemanticType *`
(inference swaps node pointers, never mutates types); `type_display()` in `src/parser.c`
renders nested types (`TYPE_ARRAY[3]:TYPE_INT`, `TYPE_SLICE:TYPE_INT`) covered by
`typed_array_nested` / `typed_slice_nested` goldens in `tests/semantic.sh` (added failing
first); `make test` and the ASan/UBSan build both green.

### M12.2 — Sized primitives and literal typing

- [x] `i8`–`i64`, `u8`–`u64`, `f32`, `f64`, `unit`, `never` kinds mapped to `stdint.h` C types
- [x] Amend LANGUAGE_SPEC §11 literal rule: context-constrained integer literals defaulting to `i64` (replaces "smallest compatible signed type")
- [x] Deterministic failing test for the `int` → `int64_t` backend migration (behavior change)
- [x] Compile-time literal range checks against resolved width (fail closed)

Evidence 2026-07-25: LANGUAGE_SPEC §11 and TYPE_SYSTEM.md literal rules amended to the
`i64` default; emitter migrated `int` → `int64_t` throughout (`%lld` printing, `LL`
literal suffix so C constant arithmetic stays 64-bit, `uint64_t` bounds checks,
`sizeof(int64_t)` slice arithmetic) behind the failing-first `i64_values` runtime test
in `tests/smoke.sh`; out-of-range literals fail closed via `ERR_LITERAL_RANGE`
(`strtoll`/`ERANGE` in `src/semantic.c`) behind the failing-first `int_literal_overflow`
/ `int_literal_overflow_expr` goldens in `tests/semantic.sh`; sized kinds `TYPE_I8`–
`TYPE_U64`, `TYPE_F32`, `TYPE_NEVER` added with `stdint.h` mappings in `emit_type_name`
(`TYPE_I64`/`TYPE_F64` alias the canonical `TYPE_INT`/`TYPE_FLOAT` so pooled types stay
unique); no surface syntax constructs sized types until M12.3 conversions. `make test`
and the ASan/UBSan build both green, including the examples suite.

### M12.3 — Explicit conversions

Status: done (2026-07-29)

- [x] `i32(x)`, `f64(n)`, etc. become real checked conversions instead of `ERR_UNSUPPORTED_CONVERSION`
- [x] Conversion allowlist; everything else keeps failing closed
- [x] No implicit narrowing or signedness change; widening only when value-preserving

Evidence 2026-07-29: conversion table in `src/semantic.c` maps `i8`–`u64`, `f32`, `f64`,
`bool`, `str` to target kinds; numeric↔numeric allowed, bool/str↔numeric rejected (E10);
arity enforced (E12); C emitter emits `((C_type)(expr))` casts; `print` accepts all sized
numeric types; width mixing without conversion rejected by `unify()` (E09). Tests:
`tests/semantic.sh` (typed_conversion_*, conversion_*, width_mixing_*), `tests/smoke.sh`
(conversion_int_f64, conversion_f64_i64, conversion_narrowing, conversion_chain,
conversion_ratio, print_i32, print_u8, print_f32). ASan/UBSan build green.

### M12.4 — Type annotation syntax

Status: done (2026-07-29)

- [x] Spec and grammar first: `param = identifier, [":", type]`, optional return annotation, `type` production
- [x] Parser `parse_type` producing type expressions resolved through the pool
- [x] Resolve `struct_def` field-type tokens through the same path (primitive types only; compound types deferred)
- [x] Recursive and exported functions require inferable-or-explicit signatures; bodies checked against declared types

Evidence 2026-07-29: GRAMMAR.md updated with `param`, `type`, `type_name`, `array_type`,
`slice_type` productions; LANGUAGE_SPEC §7 documents `param:type` and `-> type ->` return
annotation syntax; parser accepts optional `:type` after parameters and optional `-> type ->`
before body; semantic analysis resolves type annotations via `resolve_type_annot()` and
checks body against declared return type; unknown type names rejected (E09); tests:
`semantic.sh` (typed_func_annot, func_return_type_mismatch, func_unknown_type); ASan/UBSan green.

### M12.5 — Unification-based local checking

Status: done (2026-07-29)

- [x] Single `unify(expected, found)` used by binary ops, `?:` branches, call args, array elements, match arms, returns
- [x] Remove ad hoc `TYPE_UNKNOWN` in-place mutation (e.g. `len(x)` retroactively assigning slice-of-int)
- [x] Diagnostics upgraded to `expected <T>, found <U>` with source location; golden tests per error shape

Evidence 2026-07-29: `unify()` in `src/semantic.c` handles all type compatibility checks;
the `len(x)` retroactive slice inference hack (re-pointing `TYPE_UNKNOWN` symbols to
`slice-of-int`) was removed — proper forward inference from definitions is sufficient;
all existing tests pass without it. ASan/UBSan green.

### M12.6 — Composite types on the new core

Status: done (2026-07-29)

- [x] Rebase array/slice/struct types onto the arena with real nested `Type *`
- [x] Nominal identity for named types by declaration site
- [x] Struct definitions parse and emit C typedefs
- [x] Record literals construct struct values with field checking
- [x] Field access resolves against struct types with diagnostics
- [x] Option (`T?`) and Result (`T!E`) become constructible (unblocks M8) — done 2026-07-29

Evidence 2026-07-29: `struct Point { x: i64, y: i64 }` parses, registers a nominal
type via `type_get_struct()`, and emits a C typedef. Record literals `Point { x: 1, y: 2 }`
check field names and types against the definition. Field access `p.x` resolves the field
type. Diagnostics: duplicate struct, unknown struct, unknown field, field count mismatch,
field access on non-struct. Option/Result types (`some`, `none`, `ok`, `err`, `??`) implemented
in M8. ASan/UBSan green.

### M12.7 — Syntax coherence and safety audit

Status: done (2026-07-28)

Keep Tiq syntax compact while removing context-dependent rules that are easy to misread. Every accepted decision must update LANGUAGE_SPEC, GRAMMAR, examples, formatter behavior, diagnostics, and deterministic tests before implementation.

#### M12.7.1 — Close existing syntax-contract gaps (P0)

Status: done (2026-08-XX)

- [x] Singleton arrays and bracket grouping:
  - `[x]` now parses as AST_ARRAY with 1 element (not AST_BRACKET_EXPR).
  - Empty arrays `[]` rejected with ERR_EMPTY_ARRAY (E21).
  - Ordinary grouping remains `(x)`.
- [x] Array fill correctness:
  - `[x; n]` now emits explicit element list `{ x, x, ..., x }` with n copies.
  - Before: `[5; 4]` emitted `{ 5 }` which C zero-initializes the rest, producing `[5, 0, 0, 0]`.
  - After: `[5; 4]` emits `{ 5LL, 5LL, 5LL, 5LL }` which correctly initializes all elements.
- [x] Stream seed arity honesty:
  - Reject >2 seeds at semantic with E07 "stream generators support at most 2 seeds".
  - Before: silently ignored seeds 3+.
- [x] Stream bounds and predicate slicing honesty:
  - Reject bounded generators (while/until) at semantic with E07 "bounded stream generators are not yet supported".
  - Before: parsed but ignored bounds.
- [x] Block-expression contract:
  - Blocks work in function bodies.
  - Outside function bodies: E07 "block expression not supported outside function body".

#### M12.7.2 — Compact syntax decisions

- [x] Stream generator window parameters: implicit context names `a`, `b` (two seeds), `x` (one seed), `i` (index) are the v0.1 design per LANGUAGE_SPEC §14; explicit parameter names deferred to a future version.
- [x] Range-context boundary: `a..b` in non-loop/slice contexts.
  - Range expressions `a..b` are rejected outside loop brackets `[...]` or slice contexts.
  - Semantic error E07: "range expressions 'a..b' are only valid inside loop or slice contexts".
  - Added `in_range_context` flag to SemanticContext.
  - Added semantic test `range_outside_context`.
- [x] Safe partial-match policy: unmatched `match` behavior.
  - Match expressions require a wildcard arm `_ => ...`.
  - Reject at semantic with E07 "match must have a wildcard arm ('_ => ...')".
  - `_` is now a valid token (TOK_UNDERSCORE) and parses as a wildcard pattern.
  - Updated smoke test and semantic tests.
- [x] Typed function header decision: explicit type annotations are not part of v0.1; `param:type` syntax is rejected at parse time (E22) with a message directing to M12.4; LANGUAGE_SPEC §7 and GRAMMAR.md updated; `function_type_annotation` golden test added failing first.
- [x] Loop compactness audit: implicit `i` and named binders — implemented in prior commits (1beba70, 063e471): optional `j <- range` binder, `i` default, immutable loop variables.

#### M12.7.3 — Terminology and documentation surface audit

- [x] String indexing terminology: `s[i]` returns the raw byte value at position `i` as `int` (not a Unicode code point); documented in LANGUAGE_SPEC §13.1 with ASCII example and Unicode advisory.
- [x] `str` representation alignment: LANGUAGE_SPEC §11 now has a `str` representation subsection documenting the pointer+length end state and the v0.1 NUL-terminated `const char *` deviation; programs must not rely on NUL-termination (it is an implementation artifact).
- [x] Implemented-versus-reserved surface separation: LANGUAGE_SPEC §17 rewritten with a four-tier surface table (Implemented / Provisional / Fail-closed / Reserved) and subsections §17.1–§17.4 documenting each tier with error codes and blocking milestones.
- [x] Command/documentation consistency: CLI.md reconciled with main.c usage() text (fmt singular [file], test long flags, cache arg ordering, --target note, debug/inspect section, option notes table).
- [x] Syntax inventory check: GRAMMAR.md annotated every production with ✅/🟡/🔴 tier; bootstrap section cross-references LANGUAGE_SPEC §17 for the complete table; `stream_slice`/`stream_bound` (🔴), `field` (🟡), `stream_gen` (🟡), `match_expr`/`match_arm` (🟡), unary `&` (🔴) all documented.

### Exit criteria

- [x] `TYPE_SYSTEM.md` examples (`small = i8(value)`, `ratio = f64(count) / f64(total)`) compile and run
- [x] Width/signedness mixing without explicit conversion is rejected with located diagnostics
- [x] Typed IR dump shows full nested types; snapshots stable
- [x] `make test` plus sanitizer build pass

Evidence 2026-07-29: `i8(value)` and `f64(count)/f64(total)` run correctly; width mixing
tests `width_mixing_i32_i64`, `width_mixing_u8_f64` reject with E09; `dump-typed-ast`
shows `TYPE_ARRAY[3]:TYPE_INT`, `TYPE_SLICE:TYPE_INT` nested forms; ASan/UBSan green.


## Explicitly deferred

- macros;
- direct LLVM/native backend;
- garbage-collected mode;
- dynamic linking ABI stability;
- arbitrary operator overloading.


