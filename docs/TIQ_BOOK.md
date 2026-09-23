# The Tiq Book

A practical introduction to the Tiq programming language.

This guide walks you from your first Tiq program to building real tools. Each chapter builds on the last, with working code you can run immediately.

## Who this is for

You write code in C, Go, Rust, or similar languages and want a smaller, more explicit tool for command-line utilities and services. You value:

- Visible control flow and mutation
- Predictable performance without hidden runtimes
- Small syntax with useful combinations
- Compiled binaries, not interpreted scripts

## How to read this book

Every code block is a complete Tiq program. Save it as `example.tiq` and run:

```sh
./build/tiq run example.tiq
```

Or check, inspect the generated C, and build a native binary:

```sh
./build/tiq check example.tiq
./build/tiq emit-c example.tiq > example.c
./build/tiq build example.tiq -o example
./example
```

---

# Chapter 1: Your First Tiq Program

Tiq has no entry-point boilerplate. Top-level statements execute in source order.

```tiq
print("Hello, Tiq!")
```

Save this as `hello.tiq` and run it:

```sh
./build/tiq run hello.tiq
```

Output:
```
Hello, Tiq!
```

## Comments

Line comments start with `//`:

```tiq
// This is a comment
print("Hello") // This is also a comment
```

Tiq has no block comments.

## What just happened

The compiler:
1. Parsed your source
2. Type-checked it
3. Emitted C code
4. Compiled the C with your host compiler (Clang or GCC)
5. Linked and ran the binary

You can inspect the intermediate C:

```sh
./build/tiq emit-c hello.tiq
```

---

# Chapter 2: Values and Bindings

Tiq distinguishes immutable and mutable bindings explicitly.

## Immutable bindings

Use `=` to bind a value that cannot change:

```tiq
x = 42
name = "Tiq"

print(x)
print(name)
```

Attempting to reassign an immutable binding is a compile error.

## Mutable bindings

Use `<-` to declare or update a mutable binding:

```tiq
count <- 0
count <- count + 1
print(count) // 1
```

The first `<-` declares `count`. Subsequent `<-` statements update it.

## Type annotations

Types can be inferred or stated explicitly:

```tiq
x = 42           // inferred as i64
y: i32 = 100     // explicit i32
name = "Tiq"     // inferred as []u8 (byte string)
```

Type annotations use `:` after the identifier.

## Numeric types

- `i8`, `i16`, `i32`, `i64` — signed integers
- `u8`, `u16`, `u32`, `u64` — unsigned integers
- `f64` — 64-bit floating point
- `bool` — true or false

Integer literals without suffix default to `i64`. Floating-point literals default to `f64`.

---

# Chapter 3: Functions

Functions are defined with `->` separating parameters from the body.

## Simple functions

```tiq
square n -> n * n
add a b -> a + b

print(square(7))  // 49
print(add(3, 4))  // 7
```

A function returns its final expression. No `return` keyword.

## Type annotations

Parameters and return types can be explicit:

```tiq
add a: i32 b: i32 : i32 -> a + b
```

The return type annotation comes after all parameters, before `->`.

## Block bodies

Multi-line functions use `{}`:

```tiq
max a b -> {
    a > b ? a : b
}

print(max(10, 20)) // 20
```

The block's final expression is the return value.

## Calling functions

Function calls use parentheses with comma-separated arguments:

```tiq
result = add(10, 20)
print(result) // 30
```

---

# Chapter 4: Control Flow

Tiq has three control-flow constructs: conditionals, bracket loops, and while loops.

## Conditionals

The ternary operator `?` and `:` select between two branches:

```tiq
x = 10
result = x > 5 ? "big" : "small"
print(result) // big
```

For side effects, use blocks:

```tiq
x = 10
x > 5 ? {
    print("big")
} : {
    print("small")
}
```

## Bracket loops: counted iteration

The `[n <- start..end]` syntax loops over a range:

```tiq
[i <- 0..5] {
    print(i)
}
// 0
// 1
// 2
// 3
// 4
```

The range is half-open: `start..end` includes `start` but excludes `end`.

## Bracket loops: conditional

The `[condition]` syntax loops while a condition holds:

```tiq
x <- 1
[x < 100] {
    print(x)
    x <- x * 2
}
// 1
// 2
// 4
// 8
// 16
// 32
// 64
```

## Skip and break

Inside loops, `skip` continues to the next iteration, `break` exits early:

```tiq
[i <- 0..10] {
    i % 2 == 0 ? { skip }
    print(i)
}
// 1
// 3
// 5
// 7
// 9
```

```tiq
[i <- 0..100] {
    i * i > 50 ? { break }
    print(i)
}
// 0
// 1
// 2
// 3
// 4
// 5
// 6
// 7
```

---

# Chapter 5: Recurrences

Recurrences generate sequences from seeds and a rule. They are Tiq's most distinctive feature.

## Basic recurrence

```tiq
fib = [0, 1, ... (a, b) -> a + b]
print(fib[10]) // 55
```

The syntax is `[seed1, seed2, ... rule]`:
- Seeds are the initial values
- `(a, b)` names the two preceding terms (most recent first)
- The rule computes the next term

## Named index

To access the current index in the rule, use `; i`:

```tiq
factorial = [1, ... (x; i) -> x * i]
print(factorial[5]) // 120
```

Here `i` is the index being generated (starting after the seeds).

## Parameterized recurrences

A function can return a recurrence:

```tiq
powers base -> [1, ... (x) -> x * base]

p2 = powers(2)
p3 = powers(3)

print(p2[10]) // 1024
print(p3[4])  // 81
```

## How recurrences work

Recurrences are not precomputed arrays. Indexing `fib[k]` recomputes from the seeds, taking O(k) steps and O(1) state. Repeated indexing is not cached.

This makes recurrences suitable for:
- Mathematical sequences
- Lazy computation
- Memory-efficient iteration

Not suitable for:
- Random access to large sequences
- Repeated indexing of the same sequence

---

# Chapter 6: Data Structures

Tiq has two user-defined data types: structs and enums.

## Structs

A struct groups named fields:

```tiq
struct Point {
    x: i64
    y: i64
}

p = Point { x: 10, y: 20 }
print(p.x) // 10
print(p.y) // 20
```

Access fields with `.field`.

## Enums

An enum defines a set of named variants:

```tiq
enum Color {
    Red
    Green
    Blue
}

c = Color.Red
```

Enums are often used with pattern matching.

---

# Chapter 7: Pattern Matching

The `match` expression selects code based on a value's structure.

## Matching enums

```tiq
enum Light { Red, Yellow, Green }

action light -> match light {
    Light.Red => "Stop"
    Light.Yellow => "Caution"
    Light.Green => "Go"
    _ => "Unknown"
}

print(action(Light.Red))    // Stop
print(action(Light.Green))  // Go
```

Each arm is `pattern => expression`. The match must be exhaustive (cover all cases) or include a wildcard `_`.

## Wildcard patterns

```tiq
describe n -> match n {
    0 => "zero"
    1 => "one"
    _ => "many"
}

print(describe(0)) // zero
print(describe(1)) // one
print(describe(5)) // many
```

---

# Chapter 8: Error Handling

Tiq uses `Option` and `Result` types with `?` and `??` operators for error propagation and defaults.

## Option type

`Option` represents a value that may be absent:

```tiq
find x: i64 -> {
    x > 0 ? some(x) : none
}

result = find(10)
value = result ?? 0  // default if none
print(value) // 10
```

The `??` operator provides a default value.

## Result type

`Result` represents success or error:

```tiq
safe_div a: i64 b: i64 -> {
    b == 0 ? err(1) : ok(a / b)
}

r = safe_div(10, 2)
v = r ?? 0
print(v) // 5
```

## Error propagation with `?`

The `?` operator unwraps a value or returns early:

```tiq
compute a: i64 b: i64 -> {
    q = ?safe_div(a, b)
    ok(q + 10)
}

r1 = compute(10, 2)
print(r1 ?? 0) // 15

r2 = compute(10, 0)
print(r2 ?? 0) // 0 (error propagated)
```

The `?` unwraps `ok(value)` to `value`, or returns the `err` from the current function.

---

# Chapter 9: Modules and Imports

Tiq programs can span multiple files connected by `import`.

## Basic imports

```tiq
// math.tiq
add a b -> a + b
```

```tiq
// main.tiq
import "math.tiq"

print(add(10, 20)) // 30
```

All imported files share a flat global namespace.

## Standard library

Tiq has a small standard library in `std/`:

```tiq
import "std/math.tiq"
import "std/io.tiq"
```

See `docs/STDLIB.md` for available modules.

---

# Chapter 10: Working with the Compiler

The Tiq compiler (`tiq`) has several subcommands.

## Check syntax and types

```sh
./build/tiq check program.tiq
```

Reports errors without generating code.

## Emit C code

```sh
./build/tiq emit-c program.tiq > program.c
```

Generates ISO C11 code. You can inspect, compile, or modify it.

## Build a native binary

```sh
./build/tiq build program.tiq -o program
./program
```

Compiles and links a native executable.

## Run directly

```sh
./build/tiq run program.tiq
```

Compiles to a temporary location and runs immediately.

## Compiler options

- `--opt-level=0|1|2|3` — optimization level (default: 0)
- `--target=c|qbe|wasm32-wasi|native` — backend target
- `--emit-ir` — print SSA IR before code generation

See `./build/tiq --help` for all options.

---

# Chapter 11: Examples

## Fibonacci sequence

```tiq
fib = [0, 1, ... (a, b) -> a + b]
print(fib[10]) // 55
```

## Sieve of Eratosthenes

```tiq
is_prime n: i64 : bool -> {
    n <= 1 ? false : {
        res <- true
        d <- 2
        [d * d <= n && res == true] {
            n % d == 0 ? { res <- false }
            d <- d + 1
        }
        res
    }
}

[i <- 2..50] {
    is_prime(i) ? print(i)
}
```

## FizzBuzz

```tiq
[i <- 1..101] {
    i % 15 == 0 ? {
        print("FizzBuzz")
    } : i % 3 == 0 ? {
        print("Fizz")
    } : i % 5 == 0 ? {
        print("Buzz")
    } : {
        print(i)
    }
}
```

## GCD (Euclidean algorithm)

```tiq
gcd a: i64 b: i64 : i64 -> {
    b == 0 ? a : gcd(b, a % b)
}

print(gcd(48, 18)) // 6
```

---

# Next steps

- Browse `examples/` for more programs
- Read `docs/LANGUAGE_SPEC.md` for the formal specification
- Check `docs/GRAMMAR.md` for the complete grammar
- Explore `docs/DESIGN_PRINCIPLES.md` to understand Tiq's philosophy

## Community and contributions

Tiq is open source. Contributions, bug reports, and suggestions are welcome.

---

**Version:** Tiq 0.1 (pre-alpha)  
**Last updated:** 2026-09-23
