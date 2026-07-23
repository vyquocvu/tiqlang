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

Status: active

### Completed

- while loops (expression condition, block body, C emission as `while`):
  - Parsing: `while_statement()` in `parser.c` handles `while` keyword, parses condition, expects block body.
  - Semantic: checks condition is `bool`, allocates block scope for body.
  - C emission: `while (cond) { body }`; integer print via `printf("%d\n", ...)`; string print via `fputs`; bool print via `fputs(cond ? "true" : "false")`.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST, condition type error), `smoke.sh` (while count 0→3).
  - Diagnostics: missing block body reports error.

- range (for-in) loops:
  - Parsing: `for_statement()` in `parser.c` handles `for var in range { body }`.
  - Validates iterable is a `..` binary expression at parse time.
  - Semantic: creates loop variable in dedicated scope, type-checks body.
  - C emission: desugared to `for (int var = start; var < end; var++) { body }`.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST), `smoke.sh` (sum 0..4=10, print 0..2).
  - Diagnostics: missing `in`, missing range, missing block all report errors.

- break and continue:
  - Parsing: `TOK_BREAK` → `AST_BREAK`, `TOK_CONTINUE` → `AST_CONTINUE` in `statement()`.
  - Semantic: tracked via `loop_depth` in `SemanticContext`; reject break/continue outside loop.
  - C emission: `break;` and `continue;` emit directly.
  - Tests: `parser.sh` (AST golden), `semantic.sh` (typed AST, outside-loop rejection), `smoke.sh` (break exits immediately, continue skips rest).

- Expression C emitter (foundation):
  - All AST nodes now emit valid C11: `AST_LITERAL`, `AST_IDENTIFIER`, `AST_BINARY` (arithmetic, comparison, logical, bitwise), `AST_UNARY`, `AST_CONDITIONAL`, `AST_CALL`, `AST_BLOCK`, `AST_BINDING`, `AST_ASSIGN`, `AST_FUNCTION`, `AST_PRINT`.
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

### Exit criteria

- [x] while loops compile and execute correctly
- [x] range (for-in) loops compile and execute correctly
- [x] break and continue work inside loops
- [x] invalid programs are rejected (loop condition types, break outside loop)
- [x] arrays (literals, indexing, bindings, mutable element assignment, bounds checking, len built-in)
- [x] slices, string views, collection primitives (slice syntax specified in LANGUAGE_SPEC)

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
