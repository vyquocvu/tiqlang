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

Status: in progress (M13.1–M13.4 done; M13.5 active)

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
- [ ] **M13.5** C11 backend emitter in Tiq (`src/tiq/emit_c.tiq`) — active. P0 scalar core landed 2026-08-02 (`semantic_run` retained-state boundary; literals, bindings/reassignment, operators, conditionals, calls/typed `print`, bracket loops, control flow, scalar functions). P1 arrays landed 2026-08-02: typed fixed-size declarations, literals/fills, `len`, indexed reads, indexed simple/compound writes, and deterministic read/write bounds aborts matching the C reference. The emitter preflight now rejects slice nodes before writing C with a located E07 instead of emitting a placeholder. `tests/selfhost_emit_c.sh` builds the Tiq emitter, double-emits each case, compiles with strict C11 warnings-as-errors, and compares executable stdout/stderr/exit code with the bootstrap over 12 core cases plus the fail-closed slice case. Remaining before closure: slices/strings beyond literals, structs/enums, Option/Result/match, streams, borrows, ownership/defer cleanup, complete builtin/runtime prelude, module flattening, full unsupported-node coverage, and reference-C byte coverage (the final stage identity gate remains M13.6).
- [ ] **M13.6** 3-Stage Bootstrapping & Output Identity Verification
  - Stage 1: `tiq-c11` compiles `compiler.tiq` $\rightarrow$ outputs `tiq-stage1`
  - Stage 2: `tiq-stage1` compiles `compiler.tiq` $\rightarrow$ outputs `tiq-stage2`
  - Stage 3: the generated C of stage 1 and stage 2 is byte-for-byte identical (binary identity through the host C compiler is not reproducible; the deterministic artifact is the emitted C)

**Exit gate**: Clean self-hosting compiler build with zero C compiler *source* dependency. The host C compiler is still used as the backend until M17.

---

## M14 — Native Tooling in Tiq

Status: queued (prerequisites done: M13.1 language prerequisites, M13.2 lexer; does not require full self-hosting)

Depends on: M13.1 language prerequisites; M14.1–M14.3 additionally need only the Tiq lexer/front end (M13.2), so they can proceed in parallel with M13.3–M13.5 and serve as its first dogfooding programs.

Rebuild the developer tooling as Tiq programs. The original C implementations (`tiq fmt`, `tiq test`, `tiq bench`, `tiq init`, `tiq cache`, `tiq lsp`) were removed from the bootstrap compiler on 2026-07-30 to keep the C11 codebase limited to the core pipeline (lexer, parser, semantic checker, C emitter); they remain available in git history for reference.

### Tasks

- [ ] **M14.1** `tiq test`: test runner in Tiq using `//!` expected-output comments; reuses the `tests/tiq/` fixtures kept in the repository. Prioritized first: it is the harness that validates the self-hosted compiler (M13.6).
- [ ] **M14.2** `tiq fmt`: token-based formatter in Tiq (`--check`, `--output`, `--use-tabs`, `--indent-width`, stdin/stdout); needs only the lexer
- [ ] **M14.3** `tiq bench`: compiler performance measurement (lexer/parser/semantic timing, throughput, iterations); starts the continuous M21 baseline
- [ ] **M14.4** `tiq init` and package manifest handling (`*.tiq.toml`), aligned with the M18 package manager
- [ ] **M14.5** Incremental module cache, aligned with M17 native compilation and incremental builds
- [ ] **M14.6** LSP server in Tiq (JSON-RPC 2.0 over stdio, diagnostics, hover, definition, semantic tokens), feeding into M20.1

**Exit gate**: All developer tooling ships as Tiq programs; `src/` contains no C tooling code beyond the core compiler pipeline.

---

## M15 — Standard Library Modularization (`std/` Ecosystem)

Status: queued (prerequisite M13.1 module system done 2026-07-31)

Depends on: M13.1 (modules/imports, done). Must land before M19, which requires auxiliary services to live in modular standard library code.

Extract auxiliary system, networking, and serialization features from compiler intrinsics into modular `std/` packages.

### Tasks

- [ ] **M15.1** `std/fs.tiq`: File operations, directory streaming, and path manipulation
- [ ] **M15.2** `std/proc.tiq`: Process spawning, child pipes, and signal handling
- [ ] **M15.3** `std/json.tiq`: Zero-copy JSON parsing, object inspection, and string escaping
- [ ] **M15.4** `std/net.tiq`: Socket creation, listener binding, packet sending/receiving
- [ ] **M15.5** `std/ev.tiq`: Event loop abstractions and timer queue bindings

**Exit gate**: Core compiler code contains zero domain-specific builtin function names (`net_*`, `json_*`, `ev_*`).

---

## M16 — Foreign Function Interface (FFI) & C Interop System

Status: queued

Depends on: M13 (self-hosted front end to extend). Unblocks M19.6 database connectors; the C ABI design must stay compatible with the M17 native backends.

Zero-overhead C interoperability for calling host C libraries and embedding Tiq binaries into C/C++ applications.

### Tasks

- [ ] **M16.1** `extern "C"` function declaration syntax in Tiq parser & semantic analyzer
- [ ] **M16.2** C ABI type mapping for structs, pointers, and primitive scalar types
- [ ] **M16.3** Automatic C header generation tool (`tiq emit-header`) for embedding Tiq libraries into C/C++ projects
- [ ] **M16.4** Dynamic library loading (`dlopen`/`dlsym`) bindings in standard library

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
