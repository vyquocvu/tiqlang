# Post-Bootstrap & Ecosystem Maturity Roadmap

Status labels: `done`, `active`, `queued`, `blocked`.

This roadmap outlines the long-term evolution of the Tiq programming language and ecosystem following the completion of the baseline C11 bootstrap compiler (Milestones M0–M12).

Milestones are numbered in recommended execution order (renumbered on 2026-07-30; see mapping below). Each milestone states what it depends on.

## Execution order & renumbering (2026-07-30 review)

```text
M13  self-hosting prerequisites + compiler in Tiq
M14  native tooling in Tiq          (starts during M13; test runner gates M13)   [was M21]
M15  std/ modularization            (needs M13.1 modules; must precede M19)     [was M19]
M16  FFI & C interop                (unblocks M19.6 database connectors)        [was M20]
M17  native codegen & IR            (parallel track after M13)                  [was M14]
M18  package management & registry  (needs M13.1 modules and M15 packages)      [was M15]
M19  production std library & async (needs M15, M16)                            [was M16]
M20  DX, IDE tooling & spec v1.0    (needs M14.6 LSP, M17.4 wasm32)             [was M17]
M21  benchmarking & dogfooding      (continuous; starts with M14.3 bench)       [was M18]
```

Rationale for the order:

- Self-hosting (M13) is impossible without language features the bootstrap does not have yet (modules, tagged unions, growable collections); they are explicit prerequisite tasks implemented in the C bootstrap first, per the change-boundary rules.
- Tooling in Tiq (M14) comes second: the test runner is required to validate the self-hosted compiler, and the formatter/bench only need the Tiq lexer — they de-risk M13 instead of waiting for the whole ecosystem.
- Std modularization (M15) precedes the production std library (M19): M19 explicitly requires auxiliary services to live in modular standard library code, which cannot happen before `std/` packages exist.
- FFI (M16) precedes M19: database connectors (M19.6) realistically bind C client libraries.
- Benchmarking (M21) is a continuous activity, not a terminal milestone; it starts as soon as `tiq bench` returns (M14.3).

---

## M13 — Self-Hosting Compiler (Tiq in Tiq)

Status: complete (M13.1–M13.6 done 2026-08-02)

Depends on: baseline bootstrap compiler (M0–M12).

Replace the initial C11 bootstrap compiler (`src/*.c`) with a compiler written natively in Tiq (`src/tiq/*.tiq`).

### Tasks

- [x] **M13.1** Language prerequisites for self-hosting, implemented in the C bootstrap first (spec, grammar, lexer, parser, semantics, backend, diagnostics, tests per change boundaries) — closed 2026-07-31, P7 audit: `docs/M13_DETERMINISM.md` (evidence: Phase 0 `tests/determinism.sh`; P1 §19.5–§19.6; P2 §17.5 E24–E26; P3 §19.7; P4 §19.8; P5 §19.9; P6 §17.6 E27/E28 + `tests/module.sh`; P8 §19.10):
  - module system (`import`) so the compiler can span multiple `.tiq` files
  - enums / tagged unions for token kinds and AST node variants
  - growable arrays and hash maps (symbol tables, interning)
  - string builder / byte buffer for code emission
- [x] **M13.2** Lexer and AST data structures in Tiq (`src/tiq/lexer.tiq`, `src/tiq/ast.tiq`) — closed 2026-08-01: `tests/selfhost_lexer.sh` (41 fixtures) green; AST node arena is a flat `vec[int]` indexed by node id (M13.4-S3 simplification).
- [x] **M13.3** Recursive-descent parser and error reporting in Tiq (`src/tiq/parser.tiq`) — closed 2026-08-01: `tests/selfhost_parser.sh` green (41 fixtures + 41 parse-error cases + 46 construct cases). Diagnostic byte-matching vs C verified.
- [x] **M13.4** Type pool and static semantic checker in Tiq (`src/tiq/type.tiq`, `src/tiq/semantic.tiq`) — closed 2026-08-02: `type.tiq` ports the `src/type.c` pool as a flat `vec[int]` with linear interning in scan order (pool-index equality == C pointer equality); `semantic.tiq` is a ~1410-line checker; `semantic_main.tiq` reproduces `tiq dump-typed-ast`. The differential harness `tests/selfhost_semantic.sh` byte-compares stdout/stderr/exit code against the C checker over 41 fixtures + 113 semantic-error cases + 56 positive-construct cases, with non-vacuity gates (25 required `TYPE_*` names, 17 required diagnostic codes) — all green. Key fixes landed during M13.4: (1) bootstrap emitter ternary precedence bug (emit_c.c wraps `?:` in parens); (2) ASSIGN field convention (NF.A=expr, NF.B=index) across parser/semantic/dump; (3) C's generic type interner ignores inner_type, so OPTION/RESULT are not primed in the pool (first OPTION/RESULT in pool wins for `ty_get`, matching C's behavior where `none` gets `OPTION<INT>` if `some(1)` was seen first); (4) function symbol update for container returns (vec/struct keep full type, not overwritten by bare function type); (5) STREAM_GEN fail-closed on >2 seeds (untyped children, UNKNOWN type). Wired into `make test`.
- [x] **M13.5** C11 backend emitter in Tiq (`src/tiq/emit_c.tiq`) — closed 2026-08-02. The backend covers the checked language surface (scalars, arrays/slices/string views, structs/enums/match/Option/Result, streams, borrows/defer, containers, and all runtime builtins), embeds the generated bootstrap runtime, implements §16.4 owned-string cleanup including early exits, fresh-result functions, and statement temporaries, and loads normalized module graphs in deterministic DFS post-order with E27/E28 failure paths. `tests/selfhost_emit_c.sh` runs 43 executable differential cases plus strict repeat emission, ownership/free-order goldens, normalized diamond dedupe, located fail-closed/no-partial-output checks, and a whole-compiler dogfood identity gate. Both the clean normal suite and the documented ASan/UBSan suite are green. M13.6's gate is the 3-stage convergence identity below — not byte identity with the C reference: the selfhost embeds a different (Tiq-generated) runtime than the reference's prelude chunks, so cross-implementation byte identity is not an invariant; functional equivalence is pinned by the 43-case differential harness.
- [x] **M13.6** 3-Stage Bootstrapping & Output Identity Verification — closed 2026-08-02. New `tests/bootstrap.sh` (16th harness, wired directly into `make test` after `selfhost_emit_c.sh`) performs the full 3-stage convergence sequence: (1) the C bootstrap (`./build/tiq`) builds `build/tiq-stage1` from `src/tiq/emit_c_main.tiq`; (2) `tiq-stage1` emits C of its own source → `build/stage1.c`; (3) the host C compiler builds `build/tiq-stage2` from `stage1.c` using the same `-std=c11 -Os -x c` flags as the C bootstrap's `build` command, and `tiq-stage2` emits C of the same source → `build/stage2.c`. The mandatory gate `cmp -s build/stage1.c build/stage2.c` passes — the self-hosted compiler is a convergent fixed point: building it once with the C bootstrap, emitting, rebuilding from the emitted C, and emitting again produces byte-identical C (both 486,291 bytes). A sanity check feeds `tiq-stage2` a `print("ok")` fixture, compiles the emitted C, and verifies the resulting binary prints "ok" — the convergence is not just byte-identity but functional equivalence. The harness is fail-closed: missing `./build/tiq` or `src/tiq/emit_c_main.tiq` aborts with a non-zero exit and a located diagnostic, stale artifacts are removed before the run so a pass is impossible without every stage of that run producing output, and the gate cannot pass without running. Reference-vs-selfhost byte identity is explicitly **not** an M13.6 gate: the selfhost embeds a different (Tiq-generated) runtime than the C reference's prelude chunks, so the two outputs differ by design (486,291 vs 457,711 bytes for `src/tiq/emit_c_main.tiq`); the milestone's invariant is the fixed point under self-application plus differential functional equivalence. Both the clean normal suite and the documented ASan/UBSan suite are green.

**Exit gate**: Clean self-hosting compiler build with zero C compiler *source* dependency. The host C compiler is still used as the backend until M17.

---

## M14 — Native Tooling in Tiq

Status: done (M14.1–M14.6 done 2026-08-02)

Depends on: M13.1 language prerequisites; M14.1–M14.3 additionally need only the Tiq lexer/front end (M13.2), so they can proceed in parallel with M13.3–M13.5 and serve as its first dogfooding programs.

Rebuild the developer tooling as Tiq programs. The original C implementations (`tiq fmt`, `tiq test`, `tiq bench`, `tiq init`, `tiq cache`, `tiq lsp`) were removed from the bootstrap compiler on 2026-07-30 to keep the C11 codebase limited to the core pipeline (lexer, parser, semantic checker, C emitter); they remain available in git history for reference.

### Tasks

- [x] **M14.1** `tiq test`: test runner in Tiq using `//!` expected-output comments; reuses the `tests/tiq/` fixtures kept in the repository. Prioritized first: it is the harness that validates the self-hosted compiler (M13.6). Closed 2026-08-02: `src/tiq/tools/test.tiq` (~200 lines) discovers `.tiq` files non-recursively via `fs_list` (hidden names skipped), extracts expected stdout from consecutive `//! expected:` marker lines (LANGUAGE_SPEC §2.1), builds each test with a selectable compiler (`--tiq <path>`, `proc_exec`/`system`), runs it, strips trailing newlines from stdout, and requires an exact byte match. Summary `Tests: N passed, M failed, K skipped` on stdout, failures and surfaced compiler diagnostics on stderr, exit 1 iff any test failed; `--list` prints discovered test paths without running. The five `tests/tiq/*.tiq` fixtures gained `//! expected:` markers plus `print` statements so they assert real output. New `tests/test_runner.sh` harness (17th suite, wired into `make test` and the `tool-test` target) verifies: all five fixtures pass, a deliberately failing fixture exits 1 with `expected:`/`got:` on stderr, `--list` prints paths with no summary, verbose names each test, marker-less files are skipped with the `Note:` hint, a compile error surfaces the located diagnostic, a missing compiler fails closed, no transient `.testexe`/`.testout`/`.testerr` artifacts remain, and the runner's emitted C is memory-clean under ASan/UBSan. Built and verified from `make clean`; the harness was added and confirmed red before `src/tiq/tools/test.tiq` existed.
- [x] **M14.2** `tiq fmt`: token-based formatter in Tiq (`--check`, `--output`, `--use-tabs`, `--indent-width`, stdin/stdout); needs only the lexer. Closed 2026-08-02: `src/tiq/tools/fmt.tiq` (~370 lines) walks the lexer token stream (comments preserved as trivia attached to tokens) and rewrites it to the repository canonical style: tight brackets/ranges (`xs[1..3]`, `0..5`), tight call parens (`print(f(x))`), blocks glued after `]`/`->`/names, inline record literals (`Point { x: 3, y: 4 }`), spaced operators (`a + b`), ternary `?`/`:` spaced via unmatched-`?` counting, and `?[cond]` guards tight. Deterministic single-pass rewrite, final newline always inserted. CLI: reads a file path or stdin (seekable source only, e.g. `/dev/stdin` for redirects), writes to stdout or `--output <file>`; `--check` compares against the input and exits 1 with `: not formatted` on stderr when they differ; `--use-tabs`, `--indent-width <n>`; lex errors and option errors fail closed (exit 1/2). All 36 `examples/*.tiq` + `examples/leetcode/*.tiq` were normalized with `tiq fmt` itself (semantics verified byte-identical before/after) and are now `--check`-clean. New `tests/formatter_tool.sh` harness (18th suite, wired into `make test` and the `tool-fmt` target) verifies: golden outputs (loop blocks, spaced operators, ternary, stream generator, comments, struct+record literal, unary), stdin/file byte-identity, `--check` pass/fail, `--output` no-stdout, `--use-tabs`/`--indent-width 2`, idempotence, empty input, unknown flag/width/fail-closed, lex error, all examples `--check` clean, and ASan/UBSan on the formatter's emitted C. Built and verified from `make clean`; the harness was added and confirmed red before `src/tiq/tools/fmt.tiq` existed. See `docs/CLI.md` for the `tiq fmt` reference and canonical-style rules.
- [x] **M14.3** `tiq bench`: compiler performance measurement (lexer/parser/semantic timing, throughput, iterations); starts the continuous M21 baseline. Closed 2026-08-02: new `clock_ms()` builtin (LANGUAGE_SPEC §19.6 — zero-arg, returns monotonic milliseconds since an unspecified epoch, `int`-typed) added in lock-step to both compilers (C bootstrap `src/semantic.c`/`src/emit_c.c`, self-hosted `src/tiq/semantic.tiq`/`src/tiq/emit_c.tiq`) and both runtimes (`include/runtime_aux.h`, `src/tiq/emit_c_runtime.tiq` regenerated via `tools/gen_selfhost_runtime.sh`); POSIX `clock_gettime(CLOCK_MONOTONIC)` on macOS/Linux. `src/tiq/tools/bench.tiq` (~170 lines) flattens each target with `mod_flatten` (as the real drivers do), then times the self-hosted `lex_scan`/`p_parse`/`semantic_run` over `-i N` iterations of fresh driver state with `clock_ms`, reporting per-phase averages, flattened size, and end-to-end bytes/second; targets are a named `.tiq` file or a non-recursive directory scan (hidden names and non-`.tiq` files skipped, sorted order); exits 1 on unreadable files and 2 for no targets or a non-positive `-i`. New `tests/bench_tool.sh` harness (19th suite, wired into `make test` and the new `tool-bench` target) verifies the stable output shape, `-i` parsing (including `abc`/`0` fail-closed), directory scan order (a before b) and dotfile skipping, no-args/empty-dir exit 2, missing-file exit 1, and ASan/UBSan on the benchmark's emitted C. Red tests (`clock_ms` typing/arity/usage in `tests/semantic.sh`, monotonicity in `tests/run.sh`) were confirmed failing before the builtin existed. First M21 baseline recorded in `docs/OPTIMIZATION_PLAN.md`. See `docs/CLI.md` for the `tiq bench` reference.
- [x] **M14.4** `tiq init` and package manifest handling (`*.tiq.toml`), aligned with the M18 package manager. Closed 2026-08-02: new library module `src/tiq/manifest.tiq` (~220 lines) parses and validates INI-style manifests (`[package]`/`[deps]`/`[tests]` sections; `key = value` lines, `#` comments, blank lines, optional value quoting) fail-closed: unknown sections, unknown keys, duplicate `name`/`version`, a missing/empty `name`, and a `version` that is not `major.minor.patch` each produce a located `path:line: error[E30]: ...` diagnostic (E30 is a tool-local code the compiler never emits); a template generator produces the deterministic scaffold (default `name = "my-package"`, `version = "0.1.0"`, `description = "A Tiq package"`, `[tests] dir = "tests"`). `src/tiq/tools/init.tiq` (~60 lines) implements the CLI: `tiq init [name]` writes `tiq.toml` (default name) or `<name>.tiq.toml`, refuses to clobber an existing manifest (exit 1), and rejects invalid package names before touching the filesystem (exit 2); `tiq init --check <file>` validates a manifest, exiting 0 on valid, 1 with located diagnostics otherwise (an unreadable file exits 1 with `cannot read`); unknown options, `--check` without an argument, and extra arguments exit 2. New `tests/init_tool.sh` harness (20th suite, wired into `make test` and the new `tool-init` target) verifies: default and named creation with the pinned template, no-clobber, three invalid-name rejections, `--check` pass on a valid manifest, fail-closed located diagnostics for a bad version / missing name / unknown key / unknown section / unreadable file, usage errors exit 2, and ASan/UBSan on the tool's emitted C. Built and verified from `make clean`; the harness was added and confirmed red before `src/tiq/tools/init.tiq` existed. Spec: LANGUAGE_SPEC §18.2; see `docs/CLI.md` for the reference and manifest format.
- [x] **M14.5** Incremental module cache, aligned with M17 native compilation and incremental builds. Closed 2026-08-02: `src/tiq/tools/cache.tiq` (~130 lines) implements an FNV-1a content-addressed cache in `/tmp/.tiq-cache` with CLI `tiq cache clear` and `tiq cache <path>`. Cache entries are validated by comparing the stored source content hash (FNV-1a, computed in Tiq without new builtins) against the current source. `tests/cache_tool.sh` (21st harness, wired into `make test` and `tool-cache`) verifies usage fail-closed, cached/not-cached status, hash-mismatch invalidation, clear, and ASan/UBSan on the emitted C. See `docs/CLI.md` for the reference.
- [x] **M14.6** LSP server in Tiq (JSON-RPC 2.0 over stdio, diagnostics, hover, definition, semantic tokens), feeding into M20.1. Closed 2026-08-02: `src/tiq/tools/lsp.tiq` (~450 lines) implements a JSON-RPC 2.0 LSP server over stdio with Content-Length framing (seekable stdin via `/dev/stdin`). Supported methods: `initialize` (returns capabilities: textDocumentSync=1, hover, definition, semanticTokens), `shutdown`, `exit`, `textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose`, `textDocument/hover` (returns markdown type info), `textDocument/definition` (stub null), `textDocument/semanticTokens/full` (lexer-driven classification: keyword, variable, number, string). Document store uses parallel vecs (max 8 docs). Deterministic: responses depend only on document text, version, and cursor position; no environment variables or timestamps. `tests/lsp_tool.sh` (22nd harness, wired into `make test` and `tool-lsp`) verifies empty-input fail-closed (exit 1), initialize handshake, shutdown, hover, hover-null (unknown doc), semantic tokens, definition stub, and ASan/UBSan on the emitted C.

**Exit gate**: All developer tooling ships as Tiq programs; `src/` contains no C tooling code beyond the core compiler pipeline.

---

## M15 — Standard Library Modularization (`std/` Ecosystem)

Status: active (gating + first `std/` modules landed; `fs`/`proc` modularization and full builtin-name removal deferred)

Depends on: M13.1 (modules/imports, done). Must land before M19, which requires auxiliary services to live in modular standard library code.

Extract auxiliary system, networking, and serialization features from compiler intrinsics into modular `std/` packages.

### Progress (2026-08-03)

The compiler now **gates** the domain builtins (`json_*`, `net_*`, `http_*`, `ev_*`): outside a `std/` module they are no longer recognized as intrinsics, and calling one produces a located `error[E08]` with a hint naming the module to import (`import "std/json.tiq"`, etc.). User code reaches them only by importing the corresponding `std/` wrapper module. The module loader gained a cwd fallback so `import "std/<mod>.tiq"` resolves from any file depth when `tiq` runs from the project root. `json_view` and `ev_loop` remain core builtins (ungated): `json_view` returns a zero-copy `str_view` that cannot be expressed as a wrapper function return, and `ev_loop` is zero-parameter and Tiq has no zero-parameter function syntax.

### Tasks

- [ ] **M15.1** `std/fs.tiq`: File operations, directory streaming, and path manipulation — *deferred: `fs_*` stays a core builtin (used by the compiler and all tooling)*
- [ ] **M15.2** `std/proc.tiq`: Process spawning, child pipes, and signal handling — *deferred: `proc_*` stays a core builtin (used by the compiler and all tooling)*
- [x] **M15.3** `std/json.tiq`: Zero-copy JSON parsing, object inspection, and string escaping — wrappers over gated `json_*` (`json_view` stays a core builtin)
- [x] **M15.4** `std/net.tiq`: Socket creation, listener binding, packet sending/receiving — wrappers over gated `net_*`/`http_*`
- [x] **M15.5** `std/ev.tiq`: Event loop abstractions and timer queue bindings — wrappers over gated `ev_*` (`ev_loop` stays a core builtin)

**Exit gate**: Core compiler code contains zero domain-specific builtin function names (`net_*`, `json_*`, `ev_*`). *Partially met: the names are gated behind `std/` imports (no longer reachable as intrinsics from user code), but they still appear in the compiler source as the gated-builtin tables and the emitted runtime. Full physical removal of the names is deferred to M16 (FFI), which provides the mechanism to define these wrappers against real C bindings instead of compiler intrinsics.*

---

## M16 — Foreign Function Interface (FFI) & C Interop System

Status: complete (M16.1–M16.4 done 2026-08-03)

Depends on: M13 (self-hosted front end to extend). Unblocks M19.6 database connectors; the C ABI design must stay compatible with the M17 native backends.

Zero-overhead C interoperability for calling host C libraries and embedding Tiq binaries into C/C++ applications.

### Tasks

- [x] **M16.1** `extern "C"` function declaration syntax in Tiq parser & semantic analyzer — closed 2026-08-03. New reserved word `extern` (LANGUAGE_SPEC §4) and top-level production `extern_decl = "extern", string_literal, identifier, { param }, "->", type` (GRAMMAR.md), implemented in lock-step in the C bootstrap (`TOK_EXTERN`/`AST_EXTERN`, `src/parser.c extern_decl`, `src/semantic.c`) and the self-hosted compiler (`src/tiq/ast.tiq`, `lexer.tiq`, `parser.tiq`, `semantic.tiq`). The ABI operand must be exactly `"C"`; every parameter must carry a type annotation (borrow prefixes rejected, E23); the return type is mandatory; zero parameters are allowed (extern-only exception); `extern` inside a block fails closed (E05). New diagnostic E29 (`ERR_EXTERN`) covers non-`"C"` ABI, unannotated parameters, unsafe types, duplicate extern names, and name collisions with functions/structs/enums. Extern decls register in the function registry with no body, so calls type-check like user functions (arity E12, argument types E09). Evidence: `tests/parser.sh` + `tests/semantic.sh` extern goldens/negatives, `tests/ffi.sh` (24th suite, wired into `make test`), and extern cases in all three differential harnesses (`selfhost_parser.sh` 46 parse-error + 51 construct, `selfhost_semantic.sh` 126 error + 60 construct incl. E29 non-vacuity gate, `selfhost_emit_c.sh` 47 executable cases) — all byte-compared against the C reference.
- [x] **M16.2** C ABI type mapping for structs, pointers, and primitive scalar types — closed 2026-08-03. FFI-safe signature types map via the existing `emit_type_name`/`emit_semantic_type`: `i8`–`i64` → `int8_t`–`int64_t`; `u8`–`u64` → `uint8_t`–`uint64_t`; `f32`/`f64` → `float`/`double`; `bool` → `int64_t` (current backend representation); `str` → `const char *`; named structs → their emitted C typedef, passed by value. Everything else fails closed (E29). Both compilers emit `extern <ret> <name>(<params>);` (zero params → `(void)`) in declaration order immediately after the enum constants and before the stream-gen forward declarations (M13_DETERMINISM.md §1); programs without extern decls stay byte-identical. **Pointer representation decision**: v0.1 has no first-class pointer type — pointer values cross the boundary as `u64` (address-as-integer); a real pointer type is deferred to a later v0.x. **Preamble-shadow decision**: names the generated preamble's system headers already declare with unspellable signatures (`size_t`/`pid_t`/`int`/`void` returns) — `clock close exit fork getpid getppid memcmp rand read sleep strcmp strlen time write` — get no emitted prototype (a fixed-width redeclaration would conflict with the header); the header declaration serves for codegen and linking. The table is identical in `src/emit_c.c` (`ffi_shadows_preamble_header`) and `src/tiq/emit_c.tiq` (`ec_ffi_shadow`) and documented in LANGUAGE_SPEC §7.1. Linking: `tiq build`/`tiq run` gained repeatable `-l <lib>`/`-L <dir>` options forwarded to the host C compiler (capped at 16 pairs, fail-closed usage error; CLI.md). Evidence: `tests/ffi.sh` end-to-end (`llabs`/`strlen` run and print expected values, `sqrt` links with `-l m`, `emit-c` golden pins prototype lines and pass position, zero-param `(void)` shape pinned via a unique symbol, missing-symbol link failure fails closed, ASan/UBSan clean) plus the selfhost differential harness cases above.
- [x] **M16.3** Automatic C header generation tool (`tiq emit-header`) for embedding Tiq libraries into C/C++ projects — closed 2026-08-03. Two new commands, lock-step in the C bootstrap (`src/emit_c.c`, `src/main.c`) and the self-hosted compiler (`src/tiq/emit_c.tiq`, `src/tiq/emit_c_main.tiq`): `tiq emit-c --lib <file.tiq>` runs the normal emit-c pipeline but omits the generated `int main` so the translation unit links into a host program, and `tiq emit-header <file.tiq> [-o output]` emits a deterministic C header (declaration order: generator + ownership comments, `#ifndef`/`#define TIQ_<GUARD>_H` guard from the basename uppercased with non-alphanumerics → `_`, `#include <stdint.h>`, `extern "C"` guards, struct typedefs, prototypes with zero params → `(void)`). The export surface is every top-level user function whose params and return are FFI-safe per the §7.1 ABI table; vec/array/slice/map/option/result/borrow signatures, stream-gen bodies, and extern decls are skipped but stay callable from Tiq. Prototypes reuse the definition spelling path (`emit_semantic_type` in C; `ec_type` + the M16.2 `ec_ffi_param_type` table with struct lookup in self-hosted), so declaration and definition always match. Library modes enforce definitions-only modules with new diagnostic E31 (`ERR_LIBRARY`), reported at the first offending top-level statement, byte-identical in both compilers. No language change (GRAMMAR.md untouched); spec in LANGUAGE_SPEC §18.3, CLI.md updated. Evidence: `tests/ffi.sh` (header golden byte-exact incl. `-o`, skip golden for vec/borrow params, E31 fail-closed in both modes with exact diagnostic + empty stdout, usage fail-closed rc 2, `--lib` pins — no `int main(`, forward decls and extern prototypes present, `-std=c11 -c` compiles — and an end-to-end embedding build where a strict `-Wall -Wextra -Wpedantic -Werror` host C program includes the header, calls struct/str/bool/f64 exports, and prints the expected values; ASan/UBSan clean) plus `tests/selfhost_emit_c.sh` (header output byte-compared against the C reference for the golden and skip libraries, E31 diagnostic bytes identical, lib-mode structural pins on both compilers) and the M13.6 bootstrap fixed-point gate.
- [x] **M16.4** Dynamic library loading (`dlopen`/`dlsym`) bindings in standard library — closed 2026-08-03. New gated module `std/dl.tiq` (LANGUAGE_SPEC §19.11) with four builtins implemented in lock-step in the C bootstrap (`builtins[]` + `Btn` tables, `dl_` gating hint, `#include <dlfcn.h>` in the prelude, four `tiq_dl_*` helpers in `include/runtime_aux.h`) and the self-hosted compiler (`semantic.tiq` builtin chain + `is_gated_name`, `emit_c.tiq` `ec_runtime_builtin`, runtime text regenerated via `tools/gen_selfhost_runtime.sh`): `dl_open(path:str) -> u64` (`RTLD_NOW | RTLD_LOCAL`, 0 on failure), `dl_sym(handle:u64, name:str) -> u64` (0 on failure), `dl_error() -> str` (Tiq-owned copy, ungated — zero-parameter functions cannot be wrapped, same as `ev_loop`), and `dl_call(sym:u64, a..f:i64) -> i64` (generic 6-register integer-ABI call, `sym == 0` returns 0, f64 results out of scope). Runtime failures surface as 0/"" returns + `dl_error()` (no new diagnostics); compile-time errors are the standard E08/E12/E09 checks. No `-ldl` auto-append (macOS libSystem / glibc ≥ 2.34; older glibc uses `-l dl` via M16.2 forwarding); no CLI or grammar change. Along the way fixed a latent self-hosted bug surfaced by the u64 wrappers: function/extern registration passed a type-pool index where `ty_func` expects a kind — wrong for every kind ≥ 12 because Option/Result are not primed (`i32` bindings emitted as `int8_t`, etc.). Evidence: `tests/ffi.sh` (fixture dylib/so built per-platform, end-to-end `dl_call` prints 42/420, runtime fail-closed 0/0/true, compile fail-closed E08+hint/E12, ASan/UBSan clean), `tests/std_mod.sh` gate_dl + wrapper results, `tests/selfhost_emit_c.sh` m16_dl behavioral lock-step case (48 core cases) — exit gate met: C libraries are callable both via `extern "C"` + `-l` (M16.1/M16.2) and via runtime loading (M16.4).

**Exit gate**: Native C libraries (e.g. `libz`, `libssl`, `sqlite3`) can be invoked from Tiq without compiler modifications.

---

## M17 — Native Code Generation & Intermediate Representation (IR)

Status: queued

Depends on: M13 (the self-hosted compiler is the codebase that grows the IR). Runs as a parallel track; one backend at a time to avoid speculative breadth.

Direct machine code / assembly generation to bypass external C compiler host dependencies and speed up compile times.

### Tasks

- [ ] **M17.1** Static Single Assignment (SSA) IR design for Tiq
- [ ] **M17.2** One initial lightweight backend (QBE or Cranelift) for fast debug builds; the C11 emitter remains the reference backend
- [ ] **M17.3** Integrated linker / ELF / Mach-O / PE object writer
- [ ] **M17.4** Target architecture matrix (`x86_64`, `aarch64`, `riscv64`, `wasm32-wasi`); `wasm32-wasi` unblocks the M20.3 playground
- [ ] **M17.5** Optional LLVM IR backend for production release builds (`-O3`), only after the initial backend is proven

**Exit gate**: `tiq build app.tiq` produces native standalone binaries without invoking external C host compilers (`gcc`/`clang`).

---

## M18 — Package Management & Ecosystem Registry

Status: queued (M13.1 done; still blocked by M15 and M14.4)

Depends on: M13.1 (modules, done), M15 (first real packages to manage), M14.4 (manifest tooling). Local/path/git dependencies come before any central registry: a registry with zero packages is premature infrastructure.

Expand `tiq.toml` into a full-fledged package manager and central registry client.

### Tasks

- [ ] **M18.1** Manifest dependency declarations with local path and git sources
- [ ] **M18.2** Reproducible build lockfile (`tiq.lock`) with SHA-256 hash verification
- [ ] **M18.3** Package dependency resolution algorithm (PubGrub / SAT solver)
- [ ] **M18.4** Central registry protocol & server (`pkg.tiqlang.org`)
- [ ] **M18.5** Publisher tooling (`tiq publish`, `tiq login`, `tiq yank`)
- [ ] **M18.6** Security vulnerability scanning & automated dependency audit (`tiq audit`)

**Exit gate**: Developers can publish and import remote third-party Tiq packages securely with version locking.

---

## M19 — Production Standard Library & Async Core

Status: queued (blocked by M15; M19.6 additionally blocked by M16)

Depends on: M15 (`std/` packages to build on), M16 (FFI for database client libraries).

Production-grade standard library for web microservices and fast system tools. Auxiliary services (networking, event loops, JSON) are implemented cleanly in modular standard library code rather than built-in compiler intrinsics.

### Tasks

- [ ] **M19.1** High-performance non-blocking I/O event loop (`epoll` on Linux, `kqueue` on macOS, `io_uring`)
- [ ] **M19.2** Structured concurrency runtime (`spawn`, `chan`, task cancellation, structured lifetimes)
- [ ] **M19.3** Zero-copy HTTP/1.1 server and client implementation
- [ ] **M19.4** Production JSON serializer; Protocol Buffers / MessagePack as separate packages once M18 exists
- [ ] **M19.5** HTTP/2 support, once HTTP/1.1 is proven in production
- [ ] **M19.6** Standard Database Connectors (PostgreSQL, SQLite, Redis) via M16 FFI bindings

**Exit gate**: Production web service running on Tiq handling >100k req/sec with minimal memory footprint.

---

## M20 — Developer Experience, IDE Tooling & Specification v1.0

Status: queued

Depends on: M14.6 (LSP server in Tiq), M17.4 (`wasm32-wasi` for the playground). Spec v1.0 locks last, after the language has survived self-hosting and production dogfooding.

Tooling polish, formal specification lock, and documentation suite.

### Tasks

- [ ] **M20.1** Full-featured LSP server (auto-complete, refactoring, rename, inlay hints, code actions), building on M14.6
- [ ] **M20.2** Official VS Code, Neovim, and JetBrains plugins
- [ ] **M20.3** Interactive Web Playground (WASM compiler running in-browser)
- [ ] **M20.4** The Tiq Book & Interactive Documentation Portal
- [ ] **M20.5** Normative Language Specification v1.0 & Backward Compatibility Guarantee — the final lock, after everything above stops forcing syntax changes

**Exit gate**: Spec v1.0 locked with zero breaking syntax changes; complete IDE support and learning materials live.

---

## M21 — Benchmarking & Production Dogfooding (continuous)

Status: queued

Depends on: M14.3 (`tiq bench`) for the first baseline. This is a continuous activity that starts early and never closes; it is listed last only because its exit gate is measured against the finished ecosystem.

Real-world deployment and empirical performance validation.

### Tasks

- [ ] **M21.1** Continuous Performance Benchmarking Suite (Compile-time, Binary size, Memory footprint, Throughput vs C/Go/Rust/Zig), wired into CI from the first M14.3 baseline
- [ ] **M21.2** Fuzzing & Security hardening (libFuzzer / ASan continuous fuzzing pipeline), extending the existing deterministic fuzz harness
- [ ] **M21.3** Dogfooding: Build core infrastructure tools in Tiq (e.g. fast CLI tools, edge proxy)

**Exit gate**: Public benchmark suite published showing competitive performance against Rust and C with clean security audits.
