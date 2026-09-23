# Changelog

All notable changes to Tiq are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to semantic versioning once v1.0 is released.

## [Unreleased]

### Added
- IR optimization pipeline with constant folding, dead code elimination, and algebraic simplification
- `--opt-level` CLI flag to control optimization passes
- Pure-Tiq math standard library (`std/math.tiq`) with i64 and f64 core functions
- Option[T] / Result[T, E] generic type annotations
- Monomorphized Option/Result payload representation in C backend
- Self-hosted compiler driver that compiles Tiq programs end-to-end
- VS Code extension with syntax highlighting, snippets, and language configuration
- Comprehensive documentation: The Tiq Book, Tiq by Example, FAQ
- Performance benchmarks comparing Tiq with C, Go, Rust, and Python

### Fixed
- Dead code elimination correctly counts PHI node uses
- WASM emitter skips dead instructions
- Integral-only operators (`%`, `&`, `|`, `^`) rejected on floating-point operands
- Option/Result payload handling fails closed on non-i64 types

## [0.1.0] - 2026-09-23

Initial public release of the Tiq programming language.

### Core Language
- Statically typed with type inference
- Expression-based functions with explicit mutation (`=` for immutable, `<-` for mutable)
- Recurrence generators for sequence definition: `[seed, ... rule]`
- Bracket loops for iteration: `[var <- range]`
- Pattern matching with `match` expressions
- Option and Result types for error handling
- Structs and enums
- String literals with escape sequences (`\n`, `\t`, `\\`, `\"`)

### Compiler
- C11 bootstrap compiler
- Multiple backends: C (reference), QBE, WASM32-WASI, native AMD64
- Deterministic output (byte-for-byte reproducible)
- Self-hosting: Tiq compiler written in Tiq
- SSA-based intermediate representation
- Semantic analysis with clear error messages (E01-E20)

### Standard Library
- Basic I/O: `print`, `input`
- Math operations
- String manipulation
- Container types: arrays, slices
- Option and Result types

### Tooling
- `tiq run`: compile and execute in one step
- `tiq check`: type-check without generating code
- `tiq build`: compile to native binary
- `tiq emit-c`: inspect generated C code
- `tiq fmt`: format Tiq source code
- `tiq bench`: benchmark runner
- `tiq lsp`: Language Server Protocol support
- VS Code extension for syntax highlighting

### Documentation
- Language specification and grammar reference
- Design principles and rationale
- Type system and memory model documentation
- Compiler architecture overview
- CLI reference
- Performance benchmarks
- Contributing guidelines

### Known Limitations
- Pre-alpha: syntax and ABI may change
- Memory management is incomplete (ownership/borrowing in progress)
- Backend support varies (C backend is most mature)
- No Windows support yet
- Playground is static demo only (WebAssembly bridge not implemented)
- Hex and binary literals not supported

## Versioning

Tiq uses a modified versioning scheme during pre-alpha:

- **0.0.x**: Bug fixes and minor improvements
- **0.x.0**: New features and language additions
- **1.0.0**: First stable release (when syntax and ABI are frozen)

Until v1.0, breaking changes may occur between minor versions.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to contribute to Tiq.

## License

MIT License. See [LICENSE](LICENSE) for details.
