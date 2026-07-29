# Tiq

> Tiq is a tiny compiled language for fast tools and services.

Tiq is an experimental, statically typed, ahead-of-time compiled language designed around four goals:

1. **Tiny source** — remove ceremony, not familiar arithmetic and logical operators.
2. **Tiny binaries** — no mandatory garbage collector, reflection runtime, or VM.
3. **Fast builds** — a small compiler in C with a direct native compilation path.
4. **Predictable programs** — explicit mutation, strict typing, no hidden exceptions, and minimal implicit behavior.

```tiq
fib = [0, 1, ... a + b]

print(fib[10])
```

Tiq keeps familiar operators:

```text
+ - * / %
== != < <= > >=
&& || !
& | ^ << >>
+= -= *= /=
```

Language-specific syntax is intentionally small:

```text
name = value       immutable binding
name <- value     mutable binding or reassignment
f a b -> expression function
[0..10] { print(i) }  bracket loop iteration
[0, 1, ... a + b]  stream generator sequence
[1, ... x * 2 while x < 100]  bounded stream generator
skip               continue iteration shorthand
condition ? a : b  conditional expression
0..n               half-open range
print(expression)  print builtin
```

## Status

Tiq is in **pre-alpha language design and compiler bootstrap**. The syntax and ABI may change without compatibility guarantees.

The bootstrap compiler is written in ISO C11 and compiles Tiq programs into native executables through the host C compiler.

## Build the bootstrap compiler

Requirements:

- C11 compiler (`cc`, Clang, or GCC)
- POSIX-like shell for the current Makefile

```sh
make
./build/tiq --version
```

## Compiler Commands

### Build and Run

```sh
# Compile and run
./build/tiq run examples/hello.tiq

# Compile to executable
./build/tiq build examples/hello.tiq -o hello
./hello

# Compile to C (inspect generated code)
./build/tiq emit-c examples/hello.tiq
```

### Analysis Commands

```sh
# Type-check without compiling
./build/tiq check examples/*.tiq

# Dump tokens (lexer output)
./build/tiq dump-tokens examples/hello.tiq

# Dump AST (parser output)
./build/tiq dump-ast examples/hello.tiq

# Dump typed AST (semantic analysis output)
./build/tiq dump-typed-ast examples/hello.tiq
```

## Tooling Commands

### Formatter

Format Tiq source code:

```sh
# Format file to stdout
./build/tiq fmt examples/hello.tiq

# Format and write to file
./build/tiq fmt examples/hello.tiq --output formatted.tiq

# Format stdin
echo 'x=1' | ./build/tiq fmt

# Check if file is formatted (exit 0 if unchanged)
./build/tiq fmt --check examples/hello.tiq

# Use tabs for indentation
./build/tiq fmt --use-tabs examples/hello.tiq

# Custom indent width
./build/tiq fmt --indent-width 2 examples/hello.tiq
```

### Test Runner

Run tests in directories or files:

```sh
# Run all tests in directory
./build/tiq test tests/tiq/

# Run specific test file
./build/tiq test tests/tiq/hello_test.tiq

# Verbose output
./build/tiq test -v tests/tiq/

# List tests without running
./build/tiq test -l tests/tiq/
```

Test files (`.tiq`) include expected output comments using `//!`:

```tiq
// Test arithmetic
a = 1 + 2

// Test loop
total <- 0
[0..5] { total += i }

// Test fibonacci
fib = [0, 1, ... a + b]
result = fib[10]
```

##### Benchmark

Measure compiler performance:

```sh
# Benchmark single file
./build/tiq bench examples/fib.tiq

# Benchmark directory
./build/tiq bench examples/

# Verbose output with timing breakdown
./build/tiq bench -v examples/

# Multiple iterations for accuracy
./build/tiq bench -i 10 examples/

# Quiet mode (summary for CI)
./build/tiq bench -q examples/
# Output: Files: 26, Avg: 0.008 ms, Total: 0.216 ms
```

### Package Management

Initialize a new package:

```sh
# Create package manifest
./build/tiq init mypackage
# Creates: mypackage.tiq.toml

# Create default manifest
./build/tiq init
# Creates: tiq.toml
```

Manifest format (`mypackage.tiq.toml`):

```toml
# Tiq package manifest
[package]
name = "mypackage"
version = "0.1.0"
description = "A Tiq package"

[tests]
dir = "tests"
```

### Module Cache

Manage the incremental compilation cache:

```sh
# Show cache directory
./build/tiq cache path
# Output: ~/.cache/tiq

# Clear cache
./build/tiq cache clear
```

### Language Server Protocol

Run the LSP server for editor integration:

```sh
# Start LSP server (uses stdin/stdout)
./build/tiq lsp

# With custom root directory
./build/tiq lsp --root /path/to/project
```

The LSP server implements:
- JSON-RPC 2.0 over stdin/stdout
- Initialize/shutdown protocol
- Diagnostics publishing
- Text document synchronization
- Hover (symbol type information)
- Go-to-definition
- Semantic tokens (syntax highlighting)

## Running Tests

```sh
# Run all compiler tests
make test

# Run M5 tooling tests
make test-tooling

# Run all tests
make test && make test-tooling

# Run with sanitizers (recommended)
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined'
make test
```

## Repository map

```text
src/                         bootstrap compiler in C
include/                     compiler headers
examples/                    Tiq programs
tests/                       compiler and tooling tests
tests/tooling/               M5 tooling test suite
docs/LANGUAGE_SPEC.md        normative language definition
docs/GRAMMAR.md              lexical and syntactic grammar
docs/TYPE_SYSTEM.md          static type model
docs/MEMORY_MODEL.md         ownership and allocation direction
docs/COMPILER_ARCHITECTURE.md compiler pipeline and invariants
docs/ROADMAP.md              milestone plan
docs/IMPLEMENTATION_STATUS.md evidence-backed implementation state
```

## Design rule

Tiq optimizes **semantic density**, not code golf. Common operations should require little syntax, while unfamiliar punctuation and context-dependent parsing are kept under strict limits.

Read [the language specification](docs/LANGUAGE_SPEC.md) and [design principles](docs/DESIGN_PRINCIPLES.md) before proposing syntax.

## License

MIT. See [LICENSE](LICENSE).
