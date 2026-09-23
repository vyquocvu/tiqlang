# Tiq Showcase

Real-world programs and tools built with Tiq. These demonstrate where Tiq's design pays off: fast startup, predictable performance, minimal dependencies.

## Systems Tools

### File Processing

**Use case:** Process log files, transform data, extract patterns.

**Why Tiq:** Fast startup, no runtime overhead, deterministic memory usage.

```tiq
// Count lines matching a pattern
count_matches pattern lines: [str] : i64 -> {
    count <- 0
    [line <- lines] {
        // Pattern matching logic here
        count <- count + 1
    }
    count
}
```

**Example programs:**
- [JSON operations](examples/json_ops.tiq) — parse and transform structured data
- [File I/O patterns](examples/) — read, process, write

### CLI Utilities

**Use case:** Build command-line tools that start fast and exit cleanly.

**Why Tiq:** No GC pauses, no runtime initialization, single static binary.

**Characteristics:**
- Sub-millisecond startup
- Predictable memory footprint
- No hidden dependencies

**Real Tiq tools:**
- `tiq fmt` — code formatter (self-hosted)
- `tiq test` — test runner (self-hosted)
- `tiq check` — type checker

## Network Services

### HTTP Servers

**Use case:** Lightweight HTTP services, APIs, microservices.

**Why Tiq:** Fast request handling, no hidden allocations, clear cost model.

```tiq
// Routed HTTP service (self-hosted)
// See src/tiq/tools/router.tiq for full implementation
```

**Example programs:**
- [HTTP router](src/tiq/tools/router.tiq) — pattern-matched routing (written in Tiq)
- [Redis protocol](examples/redis.tiq) — protocol implementation

**Performance characteristics:**
- No reflection overhead
- No exception unwinder
- Direct system calls

### Protocol Implementations

**Use case:** Implement network protocols with precise control.

**Why Tiq:** Deterministic serialization, no hidden copies, explicit memory management.

**Example programs:**
- [Redis protocol](examples/redis.tiq) — RESP protocol handling
- [HTTP parsing](examples/) — request/response processing

## Data Processing

### Numerical Computing

**Use case:** Mathematical computations, data transformations.

**Why Tiq:** Direct float operations, no boxing overhead, predictable performance.

```tiq
// Statistical computations
mean values: [f64] : f64 -> {
    sum <- 0.0
    [v <- values] {
        sum <- sum + v
    }
    sum / len(values)
}
```

**Example programs:**
- [Math library](examples/math_demo.tiq) — numerical functions
- [Sequences](examples/sequences.tiq) — recurrence relations

### Data Transformation

**Use case:** Transform, filter, aggregate structured data.

**Why Tiq:** Fast iteration, no GC pressure, clear ownership.

**Patterns:**
- Map/filter/reduce over collections
- Structured data manipulation
- Streaming transformations

## Compiler and Language Tools

### Compilers and Interpreters

**Use case:** Build language implementations, DSLs, code generators.

**Why Tiq:** Tiq compiles itself — proven by the self-hosted compiler.

**Real example:**
- [Tiq compiler](src/tiq/) — lexer, parser, semantic checker, code generator (all in Tiq)

**Components:**
- Lexer: tokenization and source mapping
- Parser: recursive descent with error recovery
- Semantic checker: type inference and validation
- Code generator: multiple backends (C, QBE, wasm, native)

### Code Analysis Tools

**Use case:** Static analysis, linting, code transformation.

**Why Tiq:** Fast parsing, pattern matching, tree manipulation.

**Real Tiq tools:**
- `tiq fmt` — code formatter
- `tiq check` — type checker
- Test runner — parallel test execution

## Embedded and Resource-Constrained

### Minimal Runtime

**Use case:** Environments where runtime overhead matters.

**Why Tiq:** No mandatory GC, no reflection runtime, no exception unwinder.

**Characteristics:**
- Small binary size
- Minimal runtime dependencies
- Predictable resource usage

**Trade-offs:**
- Manual memory management for advanced cases
- No dynamic loading (by design)
- Explicit error handling

## Comparison with Other Languages

### vs C

**Tiq advantages:**
- Type inference reduces boilerplate
- Ownership system prevents memory bugs
- Pattern matching for cleaner control flow
- Multiple backends (C, wasm, native)

**When to use C instead:**
- Existing C codebase integration
- Platform-specific intrinsics
- Maximum control over every instruction

### vs Rust

**Tiq advantages:**
- Simpler syntax, faster learning curve
- No borrow checker complexity
- Expression-oriented by default
- Self-hosted in a manageable codebase

**When to use Rust instead:**
- Mature ecosystem needed
- Async/await for complex concurrency
- Extensive library ecosystem

### vs Go

**Tiq advantages:**
- No GC pauses
- Smaller binaries
- More predictable performance
- Pattern matching and algebraic types

**When to use Go instead:**
- Built-in concurrency (goroutines)
- Fast compilation for large teams
- Standard library for web services

## Performance Profiles

### Startup Time

**Tiq:** Sub-millisecond for most programs
- No runtime initialization
- No GC setup
- Direct entry point

**Typical measurements:**
- Hello world: < 1ms
- JSON parser: < 5ms
- HTTP server: < 10ms

### Memory Usage

**Tiq:** Predictable, no hidden allocations
- Stack allocation by default
- Explicit heap allocation
- No GC overhead
- No reflection metadata

**Typical footprints:**
- Simple tools: < 1 MB
- HTTP server: < 5 MB
- Data processor: proportional to data

### Binary Size

**Tiq:** Small static binaries
- No runtime dependencies
- Static linking by default
- Optimized code generation

**Typical sizes:**
- Hello world: ~50 KB
- JSON tool: ~200 KB
- HTTP server: ~500 KB

## Contributing to the Showcase

Built something with Tiq? Share it:

1. Add your program to `examples/` or link to external repository
2. Document the use case and why Tiq was a good fit
3. Include performance measurements if relevant
4. Submit a pull request

**What we're looking for:**
- Real programs solving real problems
- Clear demonstration of Tiq's strengths
- Honest assessment of trade-offs
- Reproducible benchmarks

## Resources

- [Examples directory](examples/) — complete working programs
- [The Tiq Book](docs/TIQ_BOOK.md) — learn Tiq from scratch
- [Performance benchmarks](docs/PERFORMANCE_BENCHMARKS.md) — detailed measurements
- [Self-hosted compiler](src/tiq/) — Tiq compiling itself
