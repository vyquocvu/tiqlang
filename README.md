# Tiq

> Tiq is a tiny compiled language for fast tools and services.

Tiq is an experimental, statically typed, ahead-of-time compiled language designed around four goals:

1. **Tiny source** — remove ceremony, not familiar arithmetic and logical operators.
2. **Tiny binaries** — no mandatory garbage collector, reflection runtime, or VM.
3. **Fast builds** — a small compiler written in C with a direct native compilation path.
4. **Predictable programs** — explicit mutation, strict typing, no hidden exceptions, and minimal implicit behavior.

```tiq
fib = [0, 1, ... a + b]

!fib[10]
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
[0..10 | !i]       bracket loop iteration
[0, 1, ... a + b]  stream generator sequence
[1, ... x * 2 while x < 100]  bounded stream generator
skip               continue iteration shorthand
condition ? a : b  conditional expression
0..n               half-open range
_                  placeholder in collection expressions
^value             early return
!value             print as a statement
```

## Status

Tiq is in **pre-alpha language design and compiler bootstrap**. The syntax and ABI may change without compatibility guarantees.

The bootstrap compiler is written in ISO C11. The first vertical slice compiles a Tiq print statement such as:

```tiq
!"Hello from Tiq"
```

into a native executable through the host C compiler.

## Build the bootstrap compiler

Requirements:

- C11 compiler (`cc`, Clang, or GCC)
- POSIX-like shell for the current Makefile

```sh
make
./build/tiq --version
```

Compile and run the first example:

```sh
./build/tiq build examples/hello.tiq -o hello
./hello
```

Inspect generated C instead:

```sh
./build/tiq emit-c examples/hello.tiq
```

## Repository map

```text
src/                         bootstrap compiler in C
include/                     compiler headers
examples/                    Tiq programs
tests/                       compiler smoke tests
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
