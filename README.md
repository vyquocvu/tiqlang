# Tiq

> Tiny source. Explicit intent. Compiled programs.

Tiq is a small, statically typed, ahead-of-time compiled language for tools and services. It removes ceremony—not familiar operators, visible bindings, or control over mutation.

A recurrence can be the program:

```tiq
fib = [0, 1, ... (a, b) -> a + b]
print(fib[10]) // 55
```

No entry-point boilerplate. No mandatory VM or garbage collector. The default compiler emits C that you can inspect, then builds a native executable with your host compiler.

**Pre-alpha:** Tiq is experimental. Syntax and ABI can change, memory management is incomplete, and backend support varies. Start with small programs, not production-critical services.

## Try Tiq

**Online (static demo):** Browse the [Tiq Playground](editors/playground/index.html) to see syntax highlighting and example programs. The playground displays Tiq code but cannot compile or run it yet (WebAssembly compiler bridge not implemented).

**Local (full compiler):** Clone this repo and run `make` — you'll have a working compiler in under a minute. See [Quick start](#quick-start) below.

[Quick start](#quick-start) · [A taste of Tiq](#a-taste-of-tiq) · [Examples](#explore-real-programs) · [Status](#what-works-today) · [Documentation](#go-deeper)

## Why Tiq?

- **Small syntax, useful combinations.** Expression functions, recurrence generators, and bracket loops express more without adding a large vocabulary.
- **Names are visible.** You name loop variables and recurrence inputs; the compiler does not invent an implicit `i`, `x`, or `it`.
- **Mutation is a choice.** `=` binds an immutable value; `<-` declares or updates a mutable binding. Arithmetic and logical operators keep their usual meanings.
- **Costs should be understandable.** No mandatory reflection runtime, exception unwinder, or hidden scheduler. Inspect the emitted C and measure the actual program.

The guiding rule is **semantic density, not code golf**: fewer repeated mechanics, not fewer clues for the reader.

## Quick start

**Prerequisites:** `make`, a C11 compiler (Clang or GCC), and a POSIX-like shell. Tested on macOS and Linux.

**Option 1: Homebrew (macOS/Linux)**

```sh
brew tap vyquocvu/tiq
brew install tiq
tiq --version
```

See `packaging/homebrew/README.md` for details.

**Option 2: Build from source**

```sh
git clone https://github.com/vyquocvu/tiqlang.git
cd tiqlang
make
./build/tiq --version
```

**Run your first program:**

```sh
./build/tiq run examples/fib.tiq
```

**Check, inspect, or keep an executable:**

```sh
./build/tiq check examples/hello.tiq
./build/tiq emit-c examples/hello.tiq
./build/tiq build examples/hello.tiq -o build/hello
./build/hello
```

**VS Code extension:** Syntax highlighting, snippets, and language configuration are available in `editors/vscode/`. Install locally with:

```sh
mkdir -p ~/.vscode/extensions/tiqlang.tiq-vscode-0.1.0
cp -r editors/vscode/* ~/.vscode/extensions/tiqlang.tiq-vscode-0.1.0/
```

Restart VS Code and open any `.tiq` file. See `editors/vscode/README.md` for details.

**Vim support:** Syntax highlighting is available in `editors/vim/`. Install with:

```sh
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp editors/vim/syntax/tiq.vim ~/.vim/syntax/
cp editors/vim/ftdetect/tiq.vim ~/.vim/ftdetect/
```

Open any `.tiq` file and syntax highlighting will activate automatically. See `editors/vim/README.md` for details.

Every Tiq block below is a complete, independent program for the default C backend. Save one as `demo.tiq` in the repository root and run `./build/tiq run demo.tiq`. Comments beside `print` show the expected output; each call prints a newline.

## A taste of Tiq

### Functions read like definitions

```tiq
square n -> n * n
larger a b -> a > b ? a : b
add a: i32 b: i32 : i32 -> a + b

print(square(7))    // 49
print(larger(3, 8)) // 8
print(add(20, 22))  // 42
```

Types can be inferred or stated. `:` introduces a type; `->` introduces the body. A function returns its final expression, including when its body is a block. Top-level statements form the program's entry point.

### Describe the recurrence, not the machinery

```tiq
factorial = [1, ... (x; i) -> x * i]
powers base -> [1, ... (x) -> x * base]

print(factorial[5])  // 120
print(powers(2)[10]) // 1024
print(powers(3)[4])  // 81
```

A generator has **seeds**, **named previous terms**, and a **rule**:

- `(x)` names the preceding term.
- `(a, b)` names the most recent term first, then the one before it. In the Fibonacci example, the first generated step binds `a = 1`, `b = 0`.
- `; i` explicitly names the index being generated; it starts after the seeds.
- A function can parameterize the rule, as `powers` does above.

These are indexed recurrences, not prebuilt arrays or general-purpose iterator pipelines. The current C backend recomputes from the seeds on each access: index `k` takes O(k) recurrence steps and O(1) recurrence state for the examples above. Repeated indexing is not cached. One or two integer seeds are supported; bounded generators and predicate slicing are not yet supported.

### One loop shape, several jobs

**Count through a range and skip selected values:**

```tiq
total <- 0
[n <- 1..11] {
    n % 2 == 0 ? { skip }
    total += n
}
print(total) // 25
```

**Repeat while a condition holds:**

```tiq
remaining <- 3
[remaining > 0] {
    print(remaining)
    remaining -= 1
}
// Output: 3, 2, 1 on separate lines.
```

**Express dependent nested loops in one header:**

```tiq
pairs <- 0
[i <- 0..4, j <- 0..i] {
    pairs += 1
}
print(pairs) // 6
```

Ranges exclude the upper bound. Multiple binders mean nested iteration, not zip: the leftmost loop is outermost, and later ranges can depend on earlier binders. `break` and `skip` target the innermost loop.

Notice the distinction: `[condition] { ... }` repeats; `condition ? { ... }` executes at most once. Both require a boolean—there is no truthiness conversion.

### Make mutation visible at the call site

```tiq
increment value: &mut i64 -> {
    value += 1
}

answer = 42
count <- 41
increment(&mut count)
print(count == answer) // true
```

`answer` cannot be reassigned. `count` can, and `&mut count` makes the function's permission to mutate it explicit. `&value` provides a shared borrow; conflicting borrows are rejected. These borrows are call-scoped, not a general-purpose reference or lifetime system.

An immutable binding does not imply deep immutability: container handles have shared mutation semantics. See the [memory model](docs/MEMORY_MODEL.md) for the limits.

### Keep failure in the return value

```tiq
half n: i64 : Result[i64, i64] ->
    n % 2 == 0 ? ok(n / 2) : err(1)

half_plus_one n: i64 : Result[i64, i64] -> {
    value = ?half(n)
    ok(value + 1)
}

print(half_plus_one(40) ?? -1) // 21
print(half_plus_one(41) ?? -1) // -1
print(none ?? 8080)           // 8080
```

`?expr` unwraps success or immediately returns the absence/error from the enclosing function. `expr ?? fallback` unwraps success or evaluates the fallback only when needed. No exceptions are involved. `some(value)` / `none` represent optional values; `ok(value)` / `err(error)` represent results.

Use `match` when the alternatives deserve names:

```tiq
reading = some(42)
message = match reading {
    some(value) => int_str(value),
    _ => "no reading"
}
print(message) // 42
```

Patterns can bind payloads. The current compiler requires a wildcard arm, even when other arms appear exhaustive. `Option[T]` and `Result[T, E]` currently support `i64` and named-struct payloads on the C backend—not unrestricted generics.

### Small records and non-owning views

```tiq
struct Point { x: i64, y: i64 }
squared_length p: Point : i64 -> p.x * p.x + p.y * p.y

print(squared_length(Point { x: 3, y: 4 })) // 25
```

```tiq
values = [10, 20, 30, 40]
middle = values[1..3]
name = "tiqlang"

print(len(middle)) // 2
print(middle[0])   // 20
print(name[..3])   // tiq
```

Slices view existing storage rather than copying it. String positions are **byte offsets**, not Unicode character indices; the underlying storage must outlive its views. Slice construction and integer-slice indexing are currently unchecked in the C backend, so keep their bounds valid.

### Reach beyond arithmetic

Import a domain library explicitly:

```tiq
import "std/json.tiq"

config = "{\"name\":\"Tiq\",\"port\":8080}"
print(json_get(config, "name")) // Tiq
print(json_get(config, "port")) // 8080
```

Or declare a C function directly:

```tiq
extern "C" llabs value: i64 : i64
print(llabs(-42)) // 42
```

The standard library includes JSON, file and process operations, containers, math, and networking facilities. Domain builtins such as JSON and networking require their matching imports. C interop supports annotated, FFI-safe signatures; external libraries can be linked with `-l` and `-L`. See the [CLI reference](docs/CLI.md) for details.

## Syntax at a glance

| Form | Meaning |
| --- | --- |
| `name = value` | Immutable binding |
| `name <- value` | Mutable binding or reassignment, never shadowing |
| `f x -> expression` | Function with inferred types |
| `f x: i64 : i64 -> x + 1` | Typed function |
| `condition ? a : b` | Choose a value |
| `condition ? { ... }` | Execute a branch once if true |
| `[i <- 0..n] { ... }` | Iterate a half-open range |
| `[condition] { ... }` | Repeat while true |
| `[1, ... (x) -> x * 2]` | Define a recurrence |
| `?result` / `result ?? default` | Propagate failure / provide a fallback |
| `xs[start..end]` | Non-owning slice |

Arithmetic, comparisons, logical operations, and bitwise operations stay familiar: `+ - * / %`, `== != < <= > >=`, `&& || !`, `& | ^ << >>`. `!` means logical negation, not printing.

## Explore real programs

| Start with | What it demonstrates |
| --- | --- |
| [Sequences](examples/sequences.tiq) | Fibonacci, factorials, squares, and powers |
| [Slices and ranges](examples/slices_and_ranges.tiq) | Views, array fills, and multi-binder loops |
| [Option and Result](examples/option_result.tiq) | Fallback and early propagation |
| [Ownership and borrows](examples/ownership_borrow.tiq) | Moves and call-scoped mutation |
| [JSON operations](examples/json_ops.tiq) | Read and update structured data |
| [C interop](examples/ffi_c_interop.tiq) | External calls and library linking |
| [Math](examples/math_demo.tiq) | Numerical library functions |

Tiq also exercises its own design: the [self-hosted compiler](src/tiq/emit_c_main.tiq), [formatter](src/tiq/tools/fmt.tiq), [test runner](src/tiq/tools/test.tiq), and [routed HTTP service](src/tiq/tools/router.tiq) are written in Tiq. These are working development artifacts, not a claim of production readiness.

## What works today

| Area | Current scope |
| --- | --- |
| Default compiler | ISO C11 bootstrap; emits C and invokes the host compiler |
| Self-hosting | Tiq-written compiler with a three-stage bootstrap fixed-point test |
| Other backends | Experimental QBE native and `wasm32-wasi` paths; feature/platform coverage differs |
| Developer tools | Tiq programs under `src/tiq/tools/`, built and tested separately—not all bootstrap subcommands |
| Compatibility | Pre-alpha; no stable source or ABI guarantee |

Important limits: no general-purpose closures, no bounded/predicate-sliced streams, no arbitrary generic types, and no complete ownership/lifetime system. Allocations and platform dependencies still exist even without a mandatory GC. C emission also has known portability gaps, including GNU-style statement expressions for matching.

See [implementation status](docs/IMPLEMENTATION_STATUS.md) for evidence and [the roadmap](docs/ROADMAP.md) for remaining work. Intended syntax in design documents should not be mistaken for a shipping feature.

## Testing and performance

The full suite additionally needs Python 3, Node.js, Perl, and a linkable SQLite3 library (including development headers on systems that package them separately). Some tests open local sockets.

```sh
make clean
make
make test
```

For parser or memory work, use sanitizers:

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined'
make test
```

Small programs and fast builds are goals, not blanket performance claims. The optional cross-language benchmark compares Tiq with optimized C, Go, Rust, and Python using checksum-verified workloads:

```sh
make benchmark-compare
```

It requires `python3`, `cc`, `go`, and `rustc`. Read the [benchmark methodology](benchmarks/language_compare/README.md) before interpreting results.

## Go deeper

- [Getting Started](docs/GETTING_STARTED.md) — step-by-step onboarding guide.
- [The Tiq Book](docs/TIQ_BOOK.md) — practical tutorial from first program to real tools.
- [Tiq by Example](docs/TIQ_BY_EXAMPLE.md) — pattern-based reference for common idioms.
- [Quick Reference](docs/QUICK_REFERENCE.md) — syntax cheat sheet for everyday use.
- [Migration Guide](docs/MIGRATION.md) — coming from C, Rust, Python, or Go.
- [Showcase](docs/SHOWCASE.md) — real-world programs and use cases.
- [FAQ](docs/FAQ.md) — frequently asked questions and troubleshooting.
- [Design principles](docs/DESIGN_PRINCIPLES.md) — why Tiq spends its syntax budget this way.
- [Language specification](docs/LANGUAGE_SPEC.md) and [grammar](docs/GRAMMAR.md) — detailed semantics and syntax.
- [Type system](docs/TYPE_SYSTEM.md) and [memory model](docs/MEMORY_MODEL.md) — constraints behind the compact surface.
- [CLI reference](docs/CLI.md) — compiler commands, backends, and developer tools.
- [Compiler architecture](docs/COMPILER_ARCHITECTURE.md) — how source becomes an executable.
- [Performance benchmarks](docs/PERFORMANCE_BENCHMARKS.md) — concrete runtime comparisons with C, Go, Rust, and Python.
- [Post-bootstrap roadmap](docs/POST_BOOTSTRAP_ROADMAP.md) and [dogfooding](docs/DOGFOODING.md) — tools, services, and readiness gates.

Repository landmarks: `src/` is the C bootstrap, `src/tiq/` the Tiq implementation and tools, `std/` the standard library, `examples/` sample programs, and `tests/` the verification suites.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to report bugs, request features, and submit code. See [CHANGELOG.md](CHANGELOG.md) for recent changes.

## License

MIT. See [LICENSE](LICENSE).
