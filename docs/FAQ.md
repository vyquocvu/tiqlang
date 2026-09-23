# Frequently Asked Questions

## Getting Started

### What do I need to build Tiq?

You need:
- `make`
- A C11 compiler (Clang or GCC)
- A POSIX-like shell (bash, zsh, sh)
- macOS or Linux

Windows is not currently supported.

### How do I build the compiler?

```sh
git clone https://github.com/vyquocvu/tiqlang.git
cd tiqlang
make
./build/tiq --version
```

### How do I run a Tiq program?

```sh
./build/tiq run examples/hello.tiq
```

Or build a native binary:

```sh
./build/tiq build examples/hello.tiq -o hello
./hello
```

### Can I use Tiq on Windows?

Not yet. Tiq currently supports macOS and Linux. Windows support requires additional backend work.

## Language Features

### Why no `return` keyword?

Tiq functions return their final expression. This keeps the syntax minimal and makes the control flow explicit. If you need early returns, use pattern matching or conditionals.

### Why no `for` or `while` loops?

Tiq uses bracket loops `[var <- range]` for iteration and recurrences for sequence generation. This makes loop variables explicit (no implicit `i`) and separates iteration from sequence definition.

### What's the difference between `=` and `<-`?

- `=` creates an immutable binding. The value cannot change.
- `<-` creates or updates a mutable binding. You can reassign with `<-`.

```tiq
x = 10      // immutable
count <- 0  // mutable
count <- 1  // ok
```

### Why do I need braces around `skip` and `break`?

In ternary expressions, `skip` and `break` must be wrapped in braces:

```tiq
condition ? { skip }   // correct
condition ? skip       // error
```

This avoids parsing ambiguity and makes the control flow explicit.

### What are recurrences?

Recurrences are sequences defined by seeds and a rule. They're Tiq's way of expressing loops and sequences declaratively:

```tiq
fib = [0, 1, ... (a, b) -> a + b]
print(fib[10])  // 55
```

The seeds are `0, 1` and the rule is `(a, b) -> a + b`.

### Can I use hex or binary literals?

Not yet. Tiq currently supports decimal integers and floats only. Hex (`0xFF`) and binary (`0b1010`) literals are not implemented.

### How do I handle errors?

Use `option T` for values that might be absent, and `result T E` for operations that can fail:

```tiq
opt = some(42)
res = ok(100)

// Use ?? for fallback
value = opt ?? 0

// Use match to destructure
msg = match res {
    ok(v) => "success",
    err(e) => "failed",
    _ => "unreachable"
}
```

## Tooling

### How do I get syntax highlighting in VS Code?

Install the VS Code extension:

```sh
mkdir -p ~/.vscode/extensions/tiqlang.tiq-vscode-0.1.0
cp -r editors/vscode/* ~/.vscode/extensions/tiqlang.tiq-vscode-0.1.0/
```

Restart VS Code and open any `.tiq` file.

### Is there a language server (LSP)?

Tiq has an LSP implementation. Run it with:

```sh
./build/tiq lsp
```

Configure your editor to connect to the LSP for hover, go-to-definition, and completion.

### Can I see the generated C code?

Yes:

```sh
./build/tiq emit-c examples/hello.tiq
```

This prints the C code that Tiq generates before compiling it.

### How do I check my program without running it?

Use `tiq check`:

```sh
./build/tiq check my_program.tiq
```

This runs the lexer, parser, and semantic analysis without generating code.

## Performance

### How fast is Tiq?

Tiq is within 5-26% of C on CPU-bound workloads, competitive with Rust, and faster than Go and Python. See the [Performance Benchmarks](PERFORMANCE_BENCHMARKS.md) for detailed comparisons.

### What are the binary sizes?

Tiq produces small binaries, typically around 50 KB for simple programs. This is much smaller than Go (2.5 MB) and comparable to C.

### Does Tiq have a garbage collector?

No. Tiq uses explicit memory management with ownership and borrowing (similar to Rust). There is no mandatory GC or runtime overhead.

## Contributing

### How do I report a bug?

Open a GitHub issue with:
- A minimal reproducible example
- Your platform (macOS/Linux) and compiler version
- The full error message

See [CONTRIBUTING.md](../CONTRIBUTING.md) for details.

### How do I add a new feature?

1. Open an issue to discuss the approach
2. Read [AGENTS.md](../AGENTS.md) for compiler invariants
3. Write a failing test first (TDD)
4. Implement the feature
5. Update the spec, grammar, and documentation

### Can I help with documentation?

Yes! Documentation improvements are always welcome. Fix typos, clarify explanations, add examples. Just make sure code examples compile with `tiq check`.

## Troubleshooting

### I get "error[E04]: expected '->' after function parameters"

This usually means you're using a reserved keyword as a variable name, or there's a syntax error in a function definition. Check that:
- Function parameters are separated by spaces, not commas
- The function body is introduced with `->`
- You're not using reserved words like `match`, `struct`, `enum` as identifiers

### I get "error[E08]: undefined symbol"

The symbol (variable, function, or type) is not in scope. Check that:
- You've declared the symbol before using it
- The spelling matches exactly (Tiq is case-sensitive)
- You're not trying to use a keyword as an identifier

### The playground doesn't run my code

The playground is currently a static demo that shows syntax but cannot compile or run code. The WebAssembly compiler bridge is not yet implemented. Use the local compiler instead:

```sh
./build/tiq run my_program.tiq
```

### Where can I get help?

- Read [The Tiq Book](TIQ_BOOK.md) for a tutorial
- Check [Tiq by Example](TIQ_BY_EXAMPLE.md) for common patterns
- Browse [examples/](../examples/) for real programs
- Open a GitHub issue for questions or bugs

## Design Philosophy

### Why is Tiq so minimal?

Tiq optimizes for **semantic density**: fewer repeated mechanics, not fewer clues for the reader. Every syntax choice has a reason. See [Design Principles](DESIGN_PRINCIPLES.md) for the full rationale.

### Why no exceptions?

Exceptions add hidden control flow and runtime overhead. Tiq uses `option` and `result` types for explicit error handling. This makes errors visible in the type system.

### Why no reflection or metaprogramming?

Reflection adds runtime overhead and complexity. Tiq is designed for tools and services where performance and predictability matter. If you need metaprogramming, Tiq may not be the right choice.

### Why emit C instead of machine code directly?

The C backend lets Tiq leverage existing C compilers (Clang, GCC) for optimization and platform support. Tiq also has a native AMD64 backend for direct machine code generation.

---

Still have questions? [Open an issue](https://github.com/vyquocvu/tiqlang/issues) or check the [documentation](README.md#go-deeper).
