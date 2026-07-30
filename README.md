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
f a:i32 -> i32 -> a + 1  typed function (param:type, -> rettype ->)
[0..10] { print(i) }  bracket loop iteration
[0, 1, ... a + b]  stream generator sequence
[1, ... x * 2 while x < 100]  bounded stream generator
skip               continue iteration shorthand
condition ? a : b  conditional expression
0..n               half-open range
print(expression)  print builtin
struct Point { x: i64, y: i64 }  struct definition
p = Point { x: 1, y: 2 }  record literal
p.x                field access
some(x) / none     Option constructors
ok(x) / err(e)     Result constructors
a ?? b             fallback (unwrap or default)
expr?              propagation (unwrap or return)
```

## Status

Tiq is in **pre-alpha language design and compiler bootstrap**. The syntax and ABI may change without compatibility guarantees.

The bootstrap compiler is written in ISO C11 and compiles Tiq programs into native executables through the host C compiler. It is intentionally limited to the core pipeline (lexer, parser, semantic checker, C emitter); developer tooling (formatter, test runner, benchmark, package manifests, module cache, LSP server) will be written in Tiq itself after self-hosting — see [the post-bootstrap roadmap](docs/POST_BOOTSTRAP_ROADMAP.md).

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

## Running Tests

```sh
# Run all compiler tests
make test

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
tests/                       compiler tests
tests/tiq/                   fixtures for the future Tiq test runner
docs/LANGUAGE_SPEC.md        normative language definition
docs/GRAMMAR.md              lexical and syntactic grammar
docs/TYPE_SYSTEM.md          static type model
docs/MEMORY_MODEL.md         ownership and allocation direction
docs/COMPILER_ARCHITECTURE.md compiler pipeline and invariants
docs/ROADMAP.md              milestone plan
docs/POST_BOOTSTRAP_ROADMAP.md post-bootstrap plan (incl. tooling in Tiq)
docs/IMPLEMENTATION_STATUS.md evidence-backed implementation state
```

## Design rule

Tiq optimizes **semantic density**, not code golf. Common operations should require little syntax, while unfamiliar punctuation and context-dependent parsing are kept under strict limits.

Read [the language specification](docs/LANGUAGE_SPEC.md) and [design principles](docs/DESIGN_PRINCIPLES.md) before proposing syntax.

## License

MIT. See [LICENSE](LICENSE).
