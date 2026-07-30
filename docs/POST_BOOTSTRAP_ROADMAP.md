# Post-Bootstrap & Ecosystem Maturity Roadmap

Status labels: `done`, `active`, `queued`, `blocked`.

This roadmap outlines the long-term evolution of the Tiq programming language and ecosystem following the completion of the baseline C11 bootstrap compiler (Milestones M0–M12).

---

## M13 — Self-Hosting Compiler (Tiq in Tiq)

Status: queued

Replace the initial C11 bootstrap compiler (`src/*.c`) with a compiler written natively in Tiq (`src/*.tiq`).

### Tasks

- [ ] **M13.1** Lexer and AST data structures in Tiq (`src/tiq/lexer.tiq`, `src/tiq/ast.tiq`)
- [ ] **M13.2** Recursive-descent parser and error reporting in Tiq (`src/tiq/parser.tiq`)
- [ ] **M13.3** Type pool and static semantic checker in Tiq (`src/tiq/type.tiq`, `src/tiq/semantic.tiq`)
- [ ] **M13.4** C11 backend emitter in Tiq (`src/tiq/emit_c.tiq`)
- [ ] **M13.5** 3-Stage Bootstrapping & Binary Identity Verification
  - Stage 1: `tiq-c11` compiles `compiler.tiq` $\rightarrow$ outputs `tiq-stage1`
  - Stage 2: `tiq-stage1` compiles `compiler.tiq` $\rightarrow$ outputs `tiq-stage2`
  - Stage 3: `diff tiq-stage1 tiq-stage2` confirms 100% byte-for-byte binary identity

**Exit gate**: Clean self-hosting compiler build with zero C host source dependency for compilation.

---

## M14 — Native Code Generation & Intermediate Representation (IR)

Status: queued

Direct machine code / assembly generation to bypass external C compiler host dependencies and speed up compile times.

### Tasks

- [ ] **M14.1** Static Single Assignment (SSA) IR design for Tiq
- [ ] **M14.2** QBE / Cranelift backend integration for ultra-fast debug builds
- [ ] **M14.3** LLVM IR backend target for production release builds (`-O3`)
- [ ] **M14.4** Target architecture matrix (`x86_64`, `aarch64`, `riscv64`, `wasm32-wasi`)
- [ ] **M14.5** Integrated linker / ELF / Mach-O / PE object writer

**Exit gate**: `tiq build app.tiq` produces native standalone binaries without invoking external C host compilers (`gcc`/`clang`).

---

## M15 — Package Management & Ecosystem Registry

Status: queued

Expand `tiq.toml` into a full-fledged package manager and central registry client.

### Tasks

- [ ] **M15.1** Package dependency resolution algorithm (PubGrub / SAT solver)
- [ ] **M15.2** Reproducible build lockfile (`tiq.lock`) with SHA-256 hash verification
- [ ] **M15.3** Central registry protocol & server (`pkg.tiqlang.org`)
- [ ] **M15.4** Publisher tooling (`tiq publish`, `tiq login`, `tiq yank`)
- [ ] **M15.5** Security vulnerability scanning & automated dependency audit (`tiq audit`)

**Exit gate**: Developers can publish and import remote third-party Tiq packages securely with version locking.

---

## M16 — Production Standard Library & Async Core

Status: queued

Production-grade standard library for web microservices and fast system tools. Auxiliary services (networking, event loops, JSON) are implemented cleanly in modular standard library code rather than built-in compiler intrinsics.

### Tasks

- [ ] **M16.1** High-performance non-blocking I/O event loop (`epoll` on Linux, `kqueue` on macOS, `io_uring`)
- [ ] **M16.2** Structured concurrency runtime (`spawn`, `chan`, task cancellation, structured lifetimes)
- [ ] **M16.3** Zero-copy HTTP/1.1 & HTTP/2 server and client implementation
- [ ] **M16.4** Production JSON / Protocol Buffers / MessagePack serializers
- [ ] **M16.5** Standard Database Connectors (PostgreSQL, SQLite, Redis)

**Exit gate**: Production web service running on Tiq handling >100k req/sec with minimal memory footprint.

---

## M17 — Developer Experience, IDE Tooling & Specification v1.0

Status: queued

Tooling polish, formal specification lock, and documentation suite.

### Tasks

- [ ] **M17.1** Normative Language Specification v1.0 & Backward Compatibility Guarantee
- [ ] **M17.2** Full-featured LSP server (auto-complete, refactoring, rename, inlay hints, code actions)
- [ ] **M17.3** Official VS Code, Neovim, and JetBrains plugins
- [ ] **M17.4** Interactive Web Playground (WASM compiler running in-browser)
- [ ] **M17.5** The Tiq Book & Interactive Documentation Portal

**Exit gate**: Spec v1.0 locked with zero breaking syntax changes; complete IDE support and learning materials live.

---

## M18 — Benchmarking & Production Dogfooding

Status: queued

Real-world deployment and empirical performance validation.

### Tasks

- [ ] **M18.1** Continuous Performance Benchmarking Suite (Compile-time, Binary size, Memory footprint, Throughput vs C/Go/Rust/Zig)
- [ ] **M18.2** Dogfooding: Build core infrastructure tools in Tiq (e.g. fast CLI tools, edge proxy)
- [ ] **M18.3** Fuzzing & Security hardening (libFuzzer / ASan continuous fuzzing pipeline)

**Exit gate**: Public benchmark suite published showing competitive performance against Rust and C with clean security audits.

---

## M19 — Standard Library Modularization (`std/` Ecosystem)

Status: queued

Extract auxiliary system, networking, and serialization features from compiler intrinsics into modular `std/` packages.

### Tasks

- [ ] **M19.1** `std/fs.tiq`: File operations, directory streaming, and path manipulation
- [ ] **M19.2** `std/proc.tiq`: Process spawning, child pipes, and signal handling
- [ ] **M19.3** `std/json.tiq`: Zero-copy JSON parsing, object inspection, and string escaping
- [ ] **M19.4** `std/net.tiq`: Socket creation, listener binding, packet sending/receiving
- [ ] **M19.5** `std/ev.tiq`: Event loop abstractions and timer queue bindings

**Exit gate**: Core compiler code contains zero domain-specific builtin function names (`net_*`, `json_*`, `ev_*`).

---

## M20 — Foreign Function Interface (FFI) & C Interop System

Status: queued

Zero-overhead C interoperability for calling host C libraries and embedding Tiq binaries into C/C++ applications.

### Tasks

- [ ] **M20.1** `extern "C"` function declaration syntax in Tiq parser & semantic analyzer
- [ ] **M20.2** C ABI type mapping for structs, pointers, and primitive scalar types
- [ ] **M20.3** Automatic C header generation tool (`tiq emit-header`) for embedding Tiq libraries into C/C++ projects
- [ ] **M20.4** Dynamic library loading (`dlopen`/`dlsym`) bindings in standard library

**Exit gate**: Native C libraries (e.g. `libz`, `libssl`, `sqlite3`) can be invoked from Tiq without compiler modifications.
