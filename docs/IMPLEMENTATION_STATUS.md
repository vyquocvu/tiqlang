# Tiq Implementation Status

Updated: 2026-07-27

## Current milestone

M12 — Type system implementation (in progress; M12.1–M12.2 complete)

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
- M1.3: Diagnostics (stable error codes `E01`-`E20` printed as `error[E0x]:`, source spans, expected/found messages).
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
  - Comments preserved as trivia tokens attached to the following token;
    re-emitted at their original line positions (comment-only lines,
    trailing comments, comments inside blocks — golden-tested in
    `tests/tooling/formatter.sh`). `make test-fmt` runs `fmt --check`
    unmasked.
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

### M7 — M11: Partial roadmap features (per ROADMAP status audit 2026-07-25)

ROADMAP is the source of truth for M7–M11 item status; the summary below mirrors its
corrected audits. None of these milestones is complete.

- M7 (active): array fill `[val; len]`, string character indexing `s[i]`, non-owning `TiqSlice` parameter decay, and block-body functions work. `chan`/`spawn` are parsed but rejected at semantic time (fail-closed, no concurrency runtime; former placeholder emission removed 2026-07-27). No `--target` flag or wasm support exists.
- M8 (active): field access (`expr.field`) and `match` parse and check; match arm types are unified since 2026-07-27 (plan 3.1). Struct definitions and record literals do NOT parse — `struct` lexes but `AST_STRUCT_DEF`/`AST_RECORD_LIT` are never constructed, so such programs fail closed with E05 (corrected 2026-07-27; blocked on M12.4 spec-and-grammar-first syntax). No Option/Result syntax exists.
- M9 (active): borrow syntax `&x` / `&mut x` parses, but since 2026-07-27 is rejected at semantic time ("borrow is not supported yet", fail closed — the backend previously emitted a silent value copy); no lifetime or aliasing validation, destructors, allocator interfaces, or `Shared<T>`.
- M10 (queued): `net_fetch` is a hardcoded stub; no event loop, sockets, or HTTP code exists. `json_parse_int`/`json_encode_str` are minimal helpers.
- M11 (queued): LSP `hover`/`definition`/`semanticTokens` answer with real front-end data since 2026-07-27 (plan 5.1): hover shows the declared symbol's inferred type, definition returns the declaration token's range, semantic tokens are delta-encoded real lexer token kinds. Responses are deterministic per stored `(uri, version)`; unknown uris/positions and unsupported methods fail closed with `null`. Structured in-protocol diagnostics, Windows platform layer, wasm playground, and self-hosted compiler remain open.

### M12: Type system implementation (in progress)

- M12.1: Type representation core (complete, 2026-07-25):
  - Interned type arena (`TypePool` in `include/type.h` / `src/type.c`): structurally identical types share one canonical `SemanticType`, so pointer equality implies type equality; pool lifetime owned by `semantic_check` callers.
  - `src/semantic.c` migrated to pooled immutable `SemanticType *`; inference propagates by swapping node type pointers instead of mutating types in place.
  - `dump_type_display()` (`src/parser.c`) renders nested composite types in `dump-typed-ast` (`TYPE_ARRAY[3]:TYPE_INT`, `TYPE_SLICE:TYPE_INT`).
  - Fixes latent uninitialized `field_count` read (pool zero-initializes types) and `param_types` leak (freed in `parser_free`).
  - Tests: `semantic.sh` (`typed_array_nested`, `typed_slice_nested` goldens, added failing first); full suite green under ASan/UBSan.
- M12.2: Sized primitives and literal typing (complete, 2026-07-25):
  - Integer values are 64-bit end to end: backend emits `int64_t` bindings, params, returns, and loop counters, `%lld` printing, `LL` literal suffixes (keeps C constant arithmetic 64-bit), `uint64_t` bounds checks, and `sizeof(int64_t)` slice arithmetic.
  - Integer literals outside `i64` are rejected at compile time (`ERR_LITERAL_RANGE`, `strtoll`/`ERANGE` in `src/semantic.c`); no executable is produced (fail closed).
  - Sized kinds `TYPE_I8`–`TYPE_U64`, `TYPE_F32`, `TYPE_NEVER` exist with `stdint.h` mappings in the emitter; `TYPE_I64`/`TYPE_F64` alias `TYPE_INT`/`TYPE_FLOAT` to keep pooled types unique. No surface syntax constructs sized types until M12.3 explicit conversions.
  - Spec: LANGUAGE_SPEC §11 and TYPE_SYSTEM.md literal rules amended to the `i64` default.
  - Tests: `smoke.sh` `i64_values` runtime test and `semantic.sh` `int_literal_overflow` / `int_literal_overflow_expr` goldens, all added failing first; full suite green under ASan/UBSan.
- Runtime helper hygiene (2026-07-27): emitted runtime helpers (`tiq_fs_write`, `tiq_fs_exists`, `tiq_proc_exec`, `tiq_proc_exit`, `tiq_json_parse_int`) return `int64_t`; `json_parse_int` uses `strtoll` so 64-bit values are not truncated (`smoke.sh` `rt_i64` goldens, added failing first).
- M12.5: Unification-based local checking (in progress, 2026-07-27):
  - Single `unify(expected, found)` in `src/semantic.c` used by binary ops, `?:` branches, builtin call args, array elements, match arms, and function return re-pointing; unknown-propagation is one rule inside it.
  - Type mismatch diagnostics print `expected <T>, found <U>` with user-facing names from `type_display()` in `src/type.c` (`int`, `str`, `[3]int`, `[]int`, ...).
  - Non-first match arms and conflicting function redefinitions are now rejected (fail closed).
  - Tests: `semantic.sh` goldens updated failing first (11 reshaped messages plus new `conditional_branch_mismatch`, `match_arm_mismatch`, `function_redefinition_mismatch`); full suite green under ASan/UBSan.
  - Remaining: retire the retroactive `len(x)`/index slice-inference symbol re-pointing (tracked in ROADMAP M12.5).
- M12.6 groundwork — nominal struct interning (2026-07-27, plan 3.2/3.3):
  - `type_get_struct(pool, name, field_names, field_types, field_count)` in `src/type.c`: struct types intern nominally by declared name (same name → same instance; same fields under a different name → distinct type); named structs are excluded from the structural intern scan.
  - `SemanticType` fixed arrays (`struct_name[64]`, `field_names[16][32]`, `field_types[16]`) replaced by pool-owned pointers; field-name strings are copied into and freed by the pool.
  - `type_display()` renders the nominal struct name in diagnostics.
  - Tests: `tests/unit/test_main.c` `test_type_pool_struct_interning` (added failing first); full suite green under ASan/UBSan (pool ownership leak-checked).
  - Not included: struct/record surface syntax, declaration-site wiring, and record-literal checking — blocked on M12.4 (spec and grammar first); the plan 3.2 record-literal goldens are unreachable until then.
- AST arena allocator (2026-07-27, plan 4.1):
  - `src/arena.c` / `include/arena.h`: bump allocator (64 KiB blocks, max-aligned, in-place growth for the newest allocation); the `Parser` owns one `Arena` that holds every `AstNode`, aux array (`call.args`, `block.statements`/`deferred`, `function.params`/`param_types`, `bracket_loop.body_stmts`, `stream_gen.seeds`, `match_expr.arms`), and the top-level statements array.
  - `parser_free` is now a single `arena_free`; the per-node partial-free switch (the source of the earlier fuzz-found leaks) is gone. Callers no longer `free(stmts)` — the parse result is arena-owned.
  - `param_types` is pre-allocated by the parser (same arena as its node); `semantic.c` only fills it in.
  - Tests: `tests/unit/test_main.c` arena growth/realloc/reset tests (added failing first, 123 assertions total); full suite + tooling + fuzz (272 inputs) green under ASan/UBSan.
  - Bench (same machine/procedure as the 0.3 baseline): total front-end time for `tiq bench -i 10 examples/` 0.208 ms → 0.126–0.137 ms (~35% faster); details in `OPTIMIZATION_PLAN.md` 4.1.
- Cache context (2026-07-27, plan 5.3):
  - `Cache` is a caller-owned struct; the `cache.c` module statics (`cache_dir`, `manifest_path`, the static path-helper buffers) are gone. `cache_entry_path` writes into a caller-provided buffer and fails closed when it would not fit.
  - Entry keys flatten path separators, fixing silent no-op `cache_put` for paths containing `/`. Dead `cache_shutdown` removed.
  - Tests: `tests/unit/test_main.c` cache context/roundtrip tests (added failing first, 138 assertions total); full suite + tooling + fuzz green under ASan/UBSan.
- LSP real symbol data (2026-07-27, plan 5.1):
  - `tiq lsp` runs the library front end (lexer+parser+semantic) on the stored `didOpen` text: hover answers `name: type` via `type_display` (functions as `fn(N) -> ret`), definition returns the declaration token's 0-based range, `semanticTokens/full` delta-encodes real lexer token kinds over the legend `[keyword, variable, number, string, operator]` declared in `initialize`.
  - Document store keyed by uri with version; requests against unopened uris, non-identifier positions, or malformed params answer `null` (fail closed). The non-protocol unsolicited startup message was removed, and header parsing now accepts `\r\n` framing (previously untested and broken). `lsp_root_path` module static replaced by a run-scoped `LspServer` context; dead `lsp_server_init`/`lsp_server_shutdown` removed from the public header.
  - Tests: golden JSON-RPC transcript `tests/tooling/lsp.sh` (added failing first, byte-exact framing compare) wired into `tests/tooling.sh`; full suite + tooling + fuzz green under ASan/UBSan.
- Doc/design review resolution (2026-07-27, `docs/DOC_REVIEW.md`):
  - Behavior: expression-position `!expr` now has type `bool` (was: operand's type; print stays a statement-level backend desugar reading the operand's type) — `typed_unary_not` golden in `tests/semantic.sh` and `not_bool` runtime test in `tests/smoke.sh`, added failing first.
  - Behavior: unary borrows `&x` / `&mut x` are rejected at semantic time ("borrow is not supported yet", E07, fail closed; previously compiled to a silent value copy) — `borrow_unsupported` / `mut_borrow_unsupported` goldens in `tests/semantic.sh`, `m9_borrow` smoke test inverted, added failing first.
  - Behavior: the undocumented stream-generator context name `s` is removed (it was never emitted for two-seed generators, producing host C compile errors); context names are now exactly `a`/`b` (two seeds), `x` (one seed), `i` (index) — `stream_state_undefined` golden in `tests/semantic.sh`, added failing first; dead `s` emission removed from `emit_c.c`.
  - Spec: LANGUAGE_SPEC gains §12 print statement (fills the missing section number), lexer-exact §4 reserved-word list, §13 array fill + string byte indexing, §14 generator context names, §15.1 error-handling design sketch (`T?`/`T!E` type constructors, short-circuiting `??` between `||` and `?:`, expression-level `expr?`, `match` destructuring), §17 provisional constructs (match, field access, rejected chan/spawn/borrow), §9 `clamp` example fixed to `->`, inline loop guards marked unimplemented.
  - Grammar: `continue` and inline guards removed from `control_stmt`; `print_stmt`, `field`, `array_fill`, `match_expr` productions added; bootstrap accept-then-reject constructs documented.
  - Docs: MEMORY_MODEL move wording aligned to the explicit `move` keyword and stale bootstrap note rewritten; TYPE_SYSTEM status header updated, `str` endgame decided (pointer+length; backend migration scheduled with M12) with the current NUL-terminated deviation documented, `!` type rule noted; CLI.md implemented list expanded to all 14 commands; README `^value` early-return and `_` placeholder lines removed, print statement added.
- Print builtin replaces the `!expr` print statement (2026-07-27, supersedes the same-day "option 3" resolution above):
  - Behavior: `print(expr)` is a builtin call (arity 1, printable argument types int/float/bool/str/str-view/slice, returns bytes written as `int`); typed printf emission moved from the statement-position `!` desugar in `emit_c.c` into the `AST_CALL` expression emitter. `!` is logical negation only, and its operand must be `bool` (`operand of '!' must be bool, found <T>`, E09; Tiq has no truthiness).
  - Tests (added failing first): `typed_print_call`, `print_no_args`, `print_two_args`, `print_unprintable`, `bang_requires_bool` goldens and the retyped `typed_unary_not` in `tests/semantic.sh`; `print_builtin` runtime output test and migrated `not_bool`/`i64_values`/`rt_i64*` in `tests/smoke.sh`; formatter call-tightness test in `tests/tooling/formatter.sh` (the formatter no longer spaces `ident (` apart and drops the dead `!`-space rule).
  - Migration: all `examples/*.tiq`, embedded programs in `tests/unit/test_main.c` and `tests/tooling/*.sh` rewritten from `!expr` to `print(expr)`.
  - Docs: LANGUAGE_SPEC §5 (`!` negation only, bool operand) and §12 (print builtin) rewritten; GRAMMAR `print_stmt` production removed; README, DESIGN_PRINCIPLES, COMPILER_ARCHITECTURE, TYPE_SYSTEM examples updated; DOC_REVIEW resolution log amended.
- Bracket loop body syntax `[domain] { body }` replaces `[domain | body]` (2026-07-27):
  - Behavior: the loop header in `[ ]` now parses a full expression (previously `bit_xor`-level, so `&&`, `||`, and bitwise `|` in the domain were parse errors despite GRAMMAR declaring `loop_domain = expression`); the body is a standard `{ }` block with newline/`;` separators (commas no longer separate body statements). `|` has no loop meaning anymore and is bitwise OR everywhere; the old form fails closed at "expected '{' to open loop body". Defer remains rejected inside loop bodies.
  - Tests (added failing first): migrated goldens plus new `bracket_loop_and_domain` in `tests/parser.sh`; `bracket_loop_no_rbracket`/`bracket_loop_no_lbrace`/`bracket_loop_no_rbrace`/`bracket_loop_old_pipe` in `tests/diagnostics.sh`; `loop_domain_expr` runtime test (`&&`/`||` headers) in `tests/smoke.sh`; `] {` same-line assertion in `tests/tooling/formatter.sh` (formatter glues the body brace to the header).
  - Migration: all loops in `examples/*.tiq`, `examples/leetcode/*.tiq`, `tests/tiq/*.tiq`, and embedded programs in `tests/tooling/*.sh` rewritten; comma body separators became `;`/newlines.
  - Docs: GRAMMAR `bracket_loop` production, LANGUAGE_SPEC §10, README, DESIGN_PRINCIPLES, ROADMAP M3 evidence updated.
- Named loop binders `[j <- 0..10] { body }` and immutable loop variables (2026-07-27):
  - Behavior: an optional `identifier <-` before a range domain names the loop variable, replacing the implicit index `i` (`IDENT '<-'` is unambiguous in header position since `<-` cannot occur inside an expression). Binders on non-range domains are rejected (`loop binder requires a range domain`, E15 — first use of the pinned code). Loop variables (binder or implicit `i`) are now immutable inside the body (E11 on assignment; previously `i` was accidentally mutable). C emission uses the binder name in the `for` header.
  - Tests (added failing first): `bracket_loop_binder` AST golden in `tests/parser.sh`; `typed_loop_binder`, `loop_binder_non_range`, `loop_index_immutable`, `loop_binder_immutable`, `loop_binder_hides_default_index` in `tests/semantic.sh`; `loop_binder` runtime test (sum + nested binders seeing outer ones) in `tests/smoke.sh`.
  - Migration: `examples/continue_skip.tiq` no longer assigns to `i` after `skip`.
  - Docs: GRAMMAR `bracket_loop` production, LANGUAGE_SPEC §10 binder + immutability rules.
- Multi-binder loops `[j <- 0..3, k <- 0..j] { body }` (2026-07-27):
  - Behavior: comma-separated binder clauses desugar in the parser to nested `AST_BRACKET_LOOP` nodes (Cartesian product), so semantic checking and C emission are unchanged; later binders see earlier ones, `break`/`skip` bind to the innermost loop. Clauses after `,` must be `name <- range` (`expected loop binder after ','`, E04) and duplicate binder names are rejected (`duplicate loop binder`, E15). No guards/filters and no zip semantics (kept out deliberately).
  - Tests (added failing first): `bracket_loop_multi_binder` desugaring golden in `tests/parser.sh`; `bracket_loop_binder_missing` and `bracket_loop_dup_binder` in `tests/diagnostics.sh`; `loop_multi_binder` runtime test (dependent ranges, sum 51) in `tests/smoke.sh`.
  - Docs: GRAMMAR `binder_clauses` production, LANGUAGE_SPEC §10 multi-binder rules.

## Known bootstrap limitations

- Temporary-file creation and host compiler execution currently use POSIX APIs; non-POSIX platforms need an equivalent documented process abstraction.
- The implementation is a proof of the compilation path, not a production compiler.
- Block bodies produce double braces in generated C (cosmetic, valid C11).
- Formatter is conservative; some stylistic variations may not be normalized.

