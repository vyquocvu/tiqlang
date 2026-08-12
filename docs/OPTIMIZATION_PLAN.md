# Tiq Compiler — Optimization & Improvement Plan

Status: draft. Every item names its evidence, its failing-test-first entry point, and the docs it must update per `AGENTS.md`. Priorities follow `DESIGN_PRINCIPLES.md`: correctness and determinism first, fast compilation second, short source fifth. Peak-performance work is evidence-gated on `tiq bench` before/after numbers.

## Phase 0 — Test and measurement foundation (prerequisite for everything else)

| # | Item | Evidence | Action | Effort |
|---|------|----------|--------|--------|
| 0.1 | C unit test harness | `tests/` contains only shell scripts; `Makefile:28-34` | Add `tests/unit/` with a single-file C11 test runner (no external deps — tiny by design). Start with lexer (token streams incl. comments), parser (AST shapes, allocation failure paths), type pool (interning identity: `type_get` twice → same pointer), env (define/lookup/shadowing). New `make test-unit` target, wired into `make test` and CI. | M |
| 0.2 | Unmask formatter CI failure | `Makefile:39` `fmt --check ... \|\| true` | Remove `\|\| true` once 1.1 lands (it would fail today). Failing test first: this is the test. | S |
| 0.3 | Bench baseline | `src/benchmark.c` exists but no recorded baseline | Record `tiq bench -i 10 examples/` numbers in this doc as the pre-optimization baseline. All Phase 4 items must cite before/after numbers in their commit evidence. | S |
| 0.4 | Fuzz the lexer and parser | No fuzzing exists | Add a deterministic, seed-driven fuzz script (`tests/fuzz.sh`): byte mutations of `examples/*.tiq` fed to `tiq check`, asserting no crash and no executable emitted on invalid input (fail-closed property). Run under ASan/UBSan. Fixed seed corpus checked in — determinism is non-negotiable. | M |

### 0.3 baseline (recorded 2026-07-27)

`tiq bench -i 10 examples/` on Apple M1 Pro, Apple clang 21.0.0, `-O2`, after fixing a
use-after-free of directory-collected file names in `benchmark.c` (regression test:
`tests/tooling/benchmark.sh` "bench directory names"):

```text
Files:      34
Min time:   0.001 ms
Max time:   0.021 ms
Avg time:   0.006 ms
Total time: 0.195 ms   (front end only: lexer + parser + semantic)
```

Inputs are tiny; per-file timings sit at timer resolution. Phase 4 items must cite
these numbers plus a like-for-like after run on the same machine.

### M14.3 / M21 baseline (recorded 2026-08-02)

The C `benchmark.c` was removed with the pre-self-hosting tooling; `tiq bench` is now
`src/tiq/tools/bench.tiq` (the self-hosted compiler phases, `mod_flatten` + `lex_scan` +
`p_parse` + `semantic_run`, timed with `clock_ms`). First M21 baseline, Apple M1 Pro,
Apple clang 21.0.0, `-O2`, `./build/tiq-bench`:

```text
examples/gcd.tiq, -i 2000 (flattened 122 bytes):
  lexer:     0 ms   parser: 0 ms   semantic: 0 ms   total: 0 ms
  throughput: 4.88 MB/s

src/tiq/lexer.tiq, -i 20 (flattened 20.8 KB):
  lexer:     15 ms   parser: 0 ms   semantic: 2 ms   total: 18 ms
  throughput: 1.16 MB/s

src/tiq/emit_c_main.tiq, -i 5 (flattened 304.9 KB, full compiler slice):
  lexer:     3161 ms   parser: 10 ms   semantic: 268 ms   total: 3440 ms
  throughput: 88.6 KB/s
```

Observations: per-phase ms values are averaged over iterations and report 0 when the
whole run is sub-millisecond; on real compiler sources the lexer dominates the front
end by ~10x over semantic analysis, and parser time is negligible. Baseline updates
must record the flattened size and exact `-i` as above; future Phase 4 items cite
before/after on the same file and iteration count.

### Cross-language runtime evidence (recorded 2026-08-12)

The checksum-verified five-million-step Park–Miller workload in
`benchmarks/language_compare/` initially measured Tiq at 68.8 ms median versus
C at 44.8 ms. Inspection showed `tiq build` deliberately used `-Os`, while the
comparison compiled C and Rust at optimization level 3. Adding the explicit
`tiq build --release` C profile and using it in the comparison reduced Tiq to
40.4 ms median (39.0 ms minimum), versus C at 41.5 ms median (39.3 ms minimum)
on the same host. This closes the measured gap without changing default
tiny-binary behavior; broader workloads are required before backend-level
optimization work is justified.

## Phase 1 — Correctness and fail-closed gaps (highest impact)

### 1.1 — Formatter must not destroy comments (data loss)

**Problem:** `lexer.c:57-65` discards `//` comments; `tiq fmt` therefore deletes them from source. Verified: `x = 1 // keep me` → `x = 1 `. Violates M5 exit criterion ("formatter processes all Tiq syntax correctly") and fail-closed.

**Action:** lex comments into trivia tokens (attach to the following token rather than the main stream, so the parser is undisturbed), and emit them from the formatter at their original line positions.

**Tests:** golden `tests/tooling/formatter.sh` cases — comment-only line, trailing comment, comment inside block — added failing first. Then remove the `Makefile:39` `|| true` (0.2).

**Updates:** `IMPLEMENTATION_STATUS.md` M5.1 note.

### 1.2 — Resolve the spawn/chan placeholder conflict with fail-closed

**Problem:** `main.c:359,362` emit `/* spawn thread */ 0` — placeholder code generation, which `DESIGN_PRINCIPLES.md` ("never silently... generate placeholder code") forbids and ROADMAP M7 admits has no runtime.

**Action:** until the M7 runtime exists, reject `spawn`/`chan` at semantic time with a located `ERR_UNSUPPORTED_*` diagnostic. This is the honest fail-closed default; re-enabling is M7 work, not this plan's.

**Tests:** `semantic.sh` goldens rejecting `spawn`/`chan` with source location, failing first. Remove/replace `smoke.sh` cases that depended on the placeholder evaluating to `0`.

**Updates:** ROADMAP M7 wording, `IMPLEMENTATION_STATUS.md`.

### 1.3 — Runtime helpers return `int` in an `int64_t` world

**Problem:** `main.c:788,802,806,811` — `tiq_fs_write`, `tiq_proc_exec`, `tiq_proc_exit`, `tiq_json_parse_int` return `int`; every call site narrows implicitly since M12.2.

**Action:** migrate to `int64_t` (do this as part of 2.2's runtime extraction so the strings move once).

**Tests:** existing `smoke.sh` built-in tests stay green; add one golden asserting `int64_t` appears in `emit-c` output for a `fs_write` call.

### 1.4 — Reconcile IMPLEMENTATION_STATUS with ROADMAP

**Problem:** `IMPLEMENTATION_STATUS.md:106-112` declares M7–M11 features complete that ROADMAP's own audit (2026-07-25) marks unimplemented.

**Action:** delete or rewrite the "M7 — M11: Complete Roadmap Features (complete)" section to mirror ROADMAP's corrected audits. Fix the `for_statement()` reference in ROADMAP M3 to `bracket_loop()`. No code change; this is an AGENTS.md compliance fix.

## Phase 2 — Architecture & code quality

### 2.1 — Split `main.c`; create `src/emit_c.c`

**Problem:** `main.c` is 1,351 lines mixing CLI, C emission, runtime templates, and stream-gen tables. `COMPILER_ARCHITECTURE.md:41-53` planned `emit_c.c` and mandated the split "by milestone M1.2"; it is now M12.

**Action**, in order (each step keeps `make test` green):

1. Move `emit_expr`, `emit_stmt`, `emit_type_name`, `emit_check_node`, `binary_op_c_str`, stream-gen table + `emit_stream_gen_def` into `src/emit_c.c` + `include/emit_c.h`. Replace the file-static `emit_stream_gen_table` (`main.c:67-68`) with an explicit `EmitContext` struct passed down — removing mutable globals makes the backend re-entrant and unit-testable (feeds 0.1).
2. Move `compile_to_c` and the stream-gen collection pass with it; `main.c` keeps only `read_all`, process/host-compiler orchestration, and CLI dispatch.
3. Fold `emit_check_node` (a separate full-AST walk) into `semantic_check` or the emitter proper — one less traversal, one less place for validation to drift.

**Tests:** no behavior change — full suite green after each move; new unit tests exercise `emit_c` against hand-built ASTs (possible only after the split).

**Updates:** `COMPILER_ARCHITECTURE.md` module list from "planned" to actual.

### 2.2 — Extract the runtime to a template file

**Problem:** ~70 lines of `fputs("...")` (`main.c:766-837`) are unreadable, unlintable C-in-strings, and the `int`/`int64_t` drift (1.3) happened because nobody reads them.

**Action:** move the runtime prelude to `src/runtime_prelude.c.inc` (plain C text, `#include`d as a string via a tiny checked-in generator or adjacent `.h` with one concatenated literal — stay C11-only, no codegen tooling). Emit with one `fputs`. Apply 1.3's `int64_t` migration here.

**Tests:** `emit-c` golden on an examples file stays byte-identical except the intended `int64_t` change (failing-first for that change).

### 2.3 — Deduplicate the formatter engines

**Problem:** `formatter.c` maintains two parallel ~85-line loops (`format_to_buf` 144-212, `format_with_lexer` 226-330) selected by entry point — a divergence trap.

**Action:** single formatting core writing through a two-function sink interface (`write_bytes`, `write_char`); buffer sink and `FILE *` sink become thin adapters. While here: replace the per-char loop in `buf_token` (141) with a bounded `memcpy` after one `buf_ensure(len)`.

**Tests:** `tests/tooling/formatter.sh` goldens plus new stdin-vs-file equivalence test (same input through both paths → identical bytes).

### 2.4 — Print error codes and retire stale ones

**Problem:** `diag.c:10` `(void)code` — 21 stable codes exist (`diag.h:6-29`) but are never printed, so "stable error codes" (M1.3, ROADMAP line 37) are not observable. `ERR_EXPECTED_PRINT`/`ERR_EXPECTED_STRING` are dead codes from the removed print statement and still drive the fatal-classification at `diag.c:18`.

**Action:** print `[E0x]`-style codes after `path:line:`; delete the two stale codes (enum reuse risk: they are internal-only, verify no golden references them); make the fatal set an explicit table, not an inline `if`.

**Tests:** update all `tests/diagnostics.sh` goldens in one mechanical pass (this is a golden-format change — do it in one commit, nothing else).

### 2.5 — Tester binary resolution

**Problem:** `tester.c:157` execs `"./build/tiq"` — breaks from any other CWD.

**Action:** resolve relative to `/proc/self/exe` (Linux) / `_NSGetExecutablePath` (macOS) behind one documented `#ifdef`, falling back to `PATH` lookup. Documented platform APIs are permitted by AGENTS.md.

**Tests:** tooling test that runs `tiq test` from `/tmp`.

## Phase 3 — Type system & semantics (roadmap-aligned)

These are not new scope — they are M12.4/M12.5/M8 items with concrete file targets.

### 3.1 — Single `unify(expected, found)` (M12.5)

**Problem:** ad-hoc kind comparisons and retroactive `sym->type` re-pointing at `semantic.c:223, 353, 505`; match takes the first arm's kind (`semantic.c:689`); ternary/binary/array-element checks each roll their own compatibility logic (e.g. lines 128-131, 196-199).

**Action:** one `unify()` used by binary ops, `?:`, call args, array elements, match arms, and returns; unknown-propagation becomes a single rule inside it. Diagnostics upgraded to `expected <T>, found <U>` using `type_display()`.

**Tests:** failing-first goldens per error shape (ROADMAP M12.5 requires this verbatim); `match_arm_mismatch` golden proving non-first arms are now checked.

**Updates:** ROADMAP M12.5 boxes, `TYPE_SYSTEM.md` if wording drifts.

### 3.2 — Struct field type resolution (M12.4 path)

**Problem:** `ast.h:146-148` stores field types as raw `Token`; `semantic.c:695-698` assigns bare `TYPE_STRUCT`; the field lookup at 658-672 is dead code today (pooled struct types never carry fields).

**Action:** route field-type tokens through the M12.4 `parse_type`/pool path; intern struct types nominally (declaration site identity, M12.6) with field metadata owned by the pool, replacing the fixed `field_names[16][32]` arrays in `semantic.h:44-47` with pool-owned slices.

**Tests:** `semantic.sh` goldens — known field type, unknown field name, wrong field type in record literal — failing first. Struct typing must fail closed on unknown field-type names.

**Updates:** ROADMAP M8 (struct row), M12.4/M12.6 boxes, LANGUAGE_SPEC struct section.

Status 2026-07-27: partially shipped. The pool half landed with 3.3 (nominal interning, pool-owned field metadata, fixed arrays removed). The semantic goldens are unreachable: source review showed `AST_STRUCT_DEF`/`AST_RECORD_LIT` are never constructed — struct/record programs fail closed with E05 today, and adding the syntax is M12.4 feature work (spec and grammar first), which this plan's non-goals exclude. ROADMAP M8 rows corrected accordingly.

### 3.3 — Type pool struct interning

**Problem:** `type.h:19-23` / `type.c:43-57` expose only primitive/func/array/slice constructors.

**Action:** `type_get_struct(pool, name_token, fields, field_count)`. Note: `type.c:19-41` `intern()` is a linear scan — fine at current pool sizes; do **not** add a hash table speculatively (see 4.4).

Status 2026-07-27: shipped (`type_get_struct` in `src/type.c`, unit-tested failing first, ASan/UBSan green).

## Phase 4 — Performance (each item gated on bench evidence from 0.3)

Ordered by expected value:

### 4.1 — AST arena allocator

**Problem:** `parser.c:18-31` one `malloc` per node plus a tracking pointer array; `parser_free` must recursively free per-node aux arrays (the recent leak fixes all lived here — `parser.c:616` etc.).

**Action:** bump-allocate `AstNode`s (and node-owned aux arrays) from one arena owned by `Parser`; `parser_free` becomes one `free`. This simultaneously removes per-node malloc traffic *and* the entire class of partial-free leaks. Deterministic: same node layout regardless of allocation pattern.

**Tests:** unit tests (0.1) for arena growth/reset; full suite + sanitizer build green; cite bench before/after.

Status 2026-07-27: shipped. `src/arena.c`/`include/arena.h` (bump allocator, 64 KiB blocks, in-place growth for the newest allocation); `Parser` owns the arena, all node and aux-array allocations route through it, `parser_free` is one `arena_free`, and callers no longer free the returned statements array. `param_types` moved from a `semantic.c` malloc to a parser-side arena allocation (same lifetime as its node). Arena growth/realloc/reset unit tests added failing first; full suite + tooling + fuzz green under ASan/UBSan. Bench, same machine and procedure as the 0.3 baseline (`tiq bench -i 10 examples/`, 34 files): before — total 0.208 ms, avg 0.006 ms, max 0.025 ms (re-measured immediately before the change; 0.3 recorded 0.195 ms); after — total 0.126–0.137 ms over three runs, avg 0.004 ms, max 0.014 ms (~35% front-end reduction). Inputs remain tiny, so per-file numbers sit at timer resolution; the total is the comparable figure.

### 4.2 — String interning for symbol and keyword comparison

**Problem:** every name comparison is `memcmp` by length — `env_lookup` (`semantic.c:49-61`), builtins table (241-249), stream-gen tables.

**Action:** one intern table in the lexer; tokens for identifiers carry an interned id; symbol keys become integer compares. Keep the table scoped to a single compilation (no global state — matches 2.1's `EmitContext` direction).

**Gate:** implement only if 0.3 baseline shows semantic analysis as a meaningful fraction on representative inputs; otherwise defer with a note here.

Status 2026-07-27: deferred per the gate. Post-4.1 measurement (`tiq bench -v -i 10` on `fib.tiq`, `primes.tiq`, `leetcode/twosum.tiq`) shows semantic analysis at roughly half of front-end time — but the absolute totals are 4–13 µs per file, at timer resolution, on every input in the repo. Attributing that half specifically to `memcmp` name comparison (vs. env setup/teardown and pool scans) would require profiling that the current inputs cannot support. Revisit when representative large inputs exist and bench shows front-end time worth optimizing.

### 4.3 — Formatter batch writes

Covered by 2.3's `memcpy` change; no separate work.

### 4.4 — Explicitly deferred performance items

Per "avoid speculative abstractions":

- **Hash table for `Environment`**: scopes hold a handful of symbols; linear scan is likely optimal at current program sizes. Revisit only with bench evidence.
- **Parser precedence-chain flattening / memoization**: 16 calls per expression is call overhead only; Pratt-table rewrite risks precedence regressions for unmeasured gain. Defer.
- **`type.c` intern hash**: pools are tiny. Defer.

## Phase 5 — Tooling

### 5.1 — LSP real symbol data (M11)

**Problem:** `lsp.c:118-131` returns hardcoded hover/definition/semanticTokens.

**Action:** after 2.1, the compiler front end is callable as a library — run lexer+parser+semantic on the open document, answer hover (symbol type via `type_display`), definition (env scope chain), semanticTokens (real token kinds from the lexer). Deterministic responses keyed to document version.

**Tests:** golden JSON-RPC transcripts in `tests/tooling/`.

Status 2026-07-27: shipped. `tiq lsp` now answers with real front-end data: `didOpen` stores the document text (keyed by uri, with version), and hover/definition/semanticTokens run lexer+parser+semantic on the stored text. Hover renders `name: type` via `type_display` (functions as `fn(N) -> ret`), definition returns the declaration token's 0-based range via a declaration index over the checked AST (closest-preceding-declaration scope approximation, forward function references resolve), and `semanticTokens/full` delta-encodes real lexer token kinds over the legend declared in `initialize`. Unknown uris, non-identifier positions, malformed params, and unsupported methods fail closed with `null`/no response. Along the way: the non-protocol unsolicited startup message was removed, header parsing now accepts `\r\n` framing (previously broken and untested), and the `lsp_root_path` module static became a run-scoped `LspServer` context (same principle as 2.1/5.3). Golden byte-exact JSON-RPC transcript `tests/tooling/lsp.sh` added failing first and wired into `tests/tooling.sh`; full suite + tooling + fuzz green under ASan/UBSan.

### 5.2 — Formatter comment + multi-line competence

Folded into 1.1 (correctness) and 2.3 (dedup). An AST-based formatter is explicitly **not** proposed: token+trivia formatting preserves layout intent with far less code, and "small syntax budget"/"short source" argue against a second tree-walking formatting engine until concrete failures demand it.

### 5.3 — Cache static buffers

**Problem:** `cache.c:15,29,35,152` static char arrays.

**Action:** caller-provided buffers. Not thread-safety-motivated (bootstrap is single-threaded) — motivated by removing hidden global state, same principle as 2.1.

Status 2026-07-27: shipped. `Cache` is now a caller-owned context (`include/cache.h`): the `cache_dir`/`manifest_path` module statics moved into the struct, and the two static-buffer path helpers were replaced by `cache_entry_path(cache, source, buf, cap)` with a caller-provided buffer that fails closed (returns false) when the path would not fit. Entry keys are flattened (`/` → `_`) so cached artifacts always land inside the cache directory — previously any source path containing `/` made `cache_put` silently write nothing. Dead `cache_shutdown` removed. Unit tests (context isolation, fail-closed small buffer, put/has/remove roundtrip) added failing first; full suite + tooling + fuzz green under ASan/UBSan.

## Roadmap alignment matrix

| Plan item | Milestone | Notes |
|---|---|---|
| 1.1 fmt comments | M5 (exit criterion repair) | shipped feature failing its own gate |
| 1.2 spawn/chan gate | M7 (honest status) | runtime itself stays queued |
| 2.1 emit_c.c split | M1.2 architecture debt + M11 (self-hosting prerequisite) | overdue per COMPILER_ARCHITECTURE.md:53 |
| 2.4 error codes printed | M1.3 completion | "stable error codes" becomes observable |
| 3.1 unify | **M12.5** (verbatim) | unblocks M8 match row |
| 3.2/3.3 struct types | **M12.4 + M12.6**, feeds M8 | field-token resolution is M12.4 line 375 |
| 4.1 arena | supports M9 (ownership work needs robust allocator story) | |
| 5.1 LSP | **M11** | |
| Borrow validation, event loop, wasm, conversions (M12.3) | M9/M10/M7 | **out of scope for this plan** — roadmap-owned feature work, not optimization |

## Non-goals (per DESIGN_PRINCIPLES / AGENTS.md)

No VM, GC, reflection, exceptions, or scheduler. No new syntax (nothing here touches GRAMMAR.md). No external dependencies, codegen tooling, or non-C11 requirements. No performance change without a recorded `tiq bench` before/after. No abstraction introduced before its second consumer exists.

## Sequencing

```text
0.1 unit harness ─┬─> 1.1 fmt comments ─> 0.2 unmask CI
0.3 bench baseline ┤
0.4 fuzz          ├─> 1.2 spawn/chan gate, 1.3 int64 runtime, 1.4 status docs
                  └─> 2.1 emit_c split ─> 2.2 runtime template ─> 2.3 fmt dedup
                                       └─> 2.4 diag codes, 2.5 tester path
                      3.1 unify ─> 3.2/3.3 structs        (M12.5 → M12.4/6 order per ROADMAP deps)
                      4.1 arena (after bench baseline; 4.2 only if measured)
                      5.x tooling (LSP after 2.1)
```

Each row ships behind `make clean && make && make test` plus the sanitizer build, with ROADMAP/IMPLEMENTATION_STATUS evidence updated in the same commit — per AGENTS.md, no exceptions.
