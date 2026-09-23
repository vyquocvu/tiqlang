# Getting Started with Tiq

A step-by-step guide to your first Tiq program.

## Prerequisites

Before you begin, ensure you have:

- **make** — build automation tool
- **C11 compiler** — Clang or GCC
- **POSIX shell** — bash, zsh, or similar
- **Git** — for cloning the repository

**Check your setup:**

```sh
make --version
clang --version  # or gcc --version
git --version
```

## Installation

### Option 1: Build from source (recommended)

```sh
# Clone the repository
git clone https://github.com/vyquocvu/tiqlang.git
cd tiqlang

# Build the compiler
make

# Verify installation
./build/tiq --version
```

### Option 2: Homebrew (macOS/Linux)

```sh
brew tap vyquocvu/tiq
brew install tiq
tiq --version
```

See [packaging/homebrew/README.md](packaging/homebrew/README.md) for details.

## Your First Program

### 1. Create a file

Create `hello.tiq`:

```tiq
print("Hello, Tiq!")
```

### 2. Run it

```sh
./build/tiq run hello.tiq
```

**Expected output:**
```
Hello, Tiq!
```

### 3. Understand what happened

1. Tiq parsed your source code
2. Type-checked all expressions
3. Generated C code (or another backend)
4. Compiled to a native executable
5. Ran the program

## Explore the Language

### Variables and Types

```tiq
// Immutable binding
name = "Tiq"

// Mutable binding
count <- 0
count <- count + 1

// With explicit types
age: i64 = 42
pi: f64 = 3.14159
```

### Functions

```tiq
// Simple function with type inference
square n -> n * n

// With explicit types
add a: i64 b: i64 : i64 -> a + b

// Call them
result = square(7)      // 49
sum = add(20, 22)       // 42
```

### Control Flow

```tiq
// Ternary operator (no if/else)
result = x > 0 ? "positive" : "non-positive"

// Pattern matching
classify n: i64 : str -> match n {
    0 => "zero",
    1 => "one",
    _ => "many"
}
```

### Loops

```tiq
// Iterate over a range
[i <- 0..10] {
    print(i)
}

// With mutable state
sum <- 0
[i <- 0..100] {
    sum <- sum + i
}
print(sum)  // 4950
```

### Recurrences

```tiq
// Fibonacci sequence
fib = [0, 1, a + b]

print(fib[0])  // 0
print(fib[1])  // 1
print(fib[2])  // 1
print(fib[3])  // 2
print(fib[4])  // 3
```

## Next Steps

### 1. Read the tutorial

[The Tiq Book](docs/TIQ_BOOK.md) walks you through the language from basics to advanced features.

### 2. Try examples

Browse the [examples directory](examples/) for complete programs:

```sh
ls examples/
./build/tiq run examples/fib.tiq
./build/tiq run examples/sequences.tiq
```

### 3. Check your understanding

Run the type checker without executing:

```sh
./build/tiq check hello.tiq
```

### 4. Inspect generated code

See what Tiq compiles to:

```sh
# Generated C code
./build/tiq emit-c hello.tiq

# SSA intermediate representation
./build/tiq emit-ir hello.tiq
```

### 5. Build an executable

Create a standalone binary:

```sh
./build/tiq build hello.tiq -o hello
./hello
```

## Learn More

### Reference materials

- [Quick Reference](docs/QUICK_REFERENCE.md) — syntax cheat sheet
- [Tiq by Example](docs/TIQ_BY_EXAMPLE.md) — pattern-based guide
- [Migration Guide](docs/MIGRATION.md) — coming from other languages
- [FAQ](docs/FAQ.md) — common questions

### Editor support

- **VS Code:** See [editors/vscode/README.md](editors/vscode/README.md)
- **Vim:** See [editors/vim/README.md](editors/vim/README.md)

### Community

- [Contributing guide](CONTRIBUTING.md) — how to help
- [CHANGELOG](CHANGELOG.md) — what's new
- [GitHub Issues](https://github.com/vyquocvu/tiqlang/issues) — report bugs

## Common Issues

### "command not found: tiq"

Use the full path: `./build/tiq`

Or add to your PATH:
```sh
export PATH="$PWD/build:$PATH"
```

### Build fails with "make: command not found"

Install build tools:
- **macOS:** `xcode-select --install`
- **Linux:** `sudo apt install build-essential` (Debian/Ubuntu) or `sudo yum groupinstall "Development Tools"` (RHEL/CentOS)

### Type errors in example code

Ensure you're using the latest compiler:
```sh
git pull
make clean
make
```

### Performance seems slow

Tiq programs compile to native code. If performance is unexpected:
1. Check you're not running in debug mode
2. Verify optimizations are enabled
3. See [Performance benchmarks](docs/PERFORMANCE_BENCHMARKS.md)

## What's Next?

Once you're comfortable with the basics:

1. **Build something:** Create a small tool or utility
2. **Read the spec:** [Language specification](docs/LANGUAGE_SPEC.md)
3. **Explore internals:** [Compiler architecture](docs/COMPILER_ARCHITECTURE.md)
4. **Contribute:** Fix a bug, add an example, improve docs

## Getting Help

- **Documentation:** Check the [FAQ](docs/FAQ.md) first
- **Examples:** Browse [examples/](examples/) for working code
- **Issues:** Report bugs on [GitHub](https://github.com/vyquocvu/tiqlang/issues)

Welcome to Tiq!
