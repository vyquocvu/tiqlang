# Tiq Quick Reference

A one-page cheat sheet for Tiq syntax. See [The Tiq Book](TIQ_BOOK.md) for tutorials and [Tiq by Example](TIQ_BY_EXAMPLE.md) for complete programs.

## Comments

```tiq
// Line comment
```

## Bindings

```tiq
x = 42          // Immutable
y <- 10         // Mutable
y <- 20         // Update
name: str = "tiq"  // With type
```

## Types

| Type | Description |
|------|-------------|
| `i64` | 64-bit signed integer |
| `i32` | 32-bit signed integer |
| `f64` | 64-bit float |
| `bool` | Boolean (`true`, `false`) |
| `str` | String |
| `void` | No value |
| `[T]` | Vec: dynamic array of T |
| `Option[T]` | Some value or None |
| `Result[T, E]` | Ok value or Err |

## Functions

```tiq
// Inferred types
square n -> n * n

// Explicit types
add a: i64 b: i64 : i64 -> a + b

// Block body
classify n: i64 : str -> {
    n > 0 ? "positive" : "negative"
}

// Call
result = add(10, 20)
```

## Operators

```tiq
// Arithmetic
+ - * / %

// Comparison
== != < > <= >=

// Logical
&& || !

// Bitwise
& | ^ ~ << >>

// Fallback (Option/Result only)
value ?? default
```

## Control Flow

```tiq
// Ternary (no if/else)
result = condition ? value_if_true : value_if_false

// With blocks
result = x > 0 ? { positive } : { negative }
```

## Loops

```tiq
// Bracket loop with range
[i <- 0..10] {
    print(i)
}

// With binding
[i <- items] {
    print(i)
}

// Skip and break (must be in braces)
[i <- 0..10] {
    i == 5 ? { skip } : print(i)
    i == 8 ? { break } : {}
}
```

## Recurrences

```tiq
// Fibonacci
fib = [0, 1, a + b]

// Access elements
print(fib[0])  // 0
print(fib[1])  // 1
print(fib[2])  // 1
print(fib[3])  // 2

// Factorial
fact = [1, n * prev]

// Custom seed and rule
seq = [1, 2, prev_a + prev_b * 2]
```

## Structs

```tiq
struct Point {
    x: i64,
    y: i64
}

p = Point { x: 10, y: 20 }
print(p.x)
```

## Enums

```tiq
enum Color {
    Red,
    Green,
    Blue
}

c = Red
```

## Pattern Matching

```tiq
result = match value {
    0 => "zero",
    1 => "one",
    _ => "other"  // Wildcard required
}

// With enum
color_name = match c {
    Red => "red",
    Green => "green",
    Blue => "blue"
}
```

## Option and Result

```tiq
// Option
opt_val = some(42)
unwrapped = opt_val ?? 0  // 42 or default

// Result
res = ok(100)
res_err = err("failed")

// Fallback
value = res ?? 0
```

## Vec Operations

```tiq
v = [1, 2, 3]
print(v[0])      // Index
print(len(v))    // Length
```

## String Escapes

```tiq
"newline\n"
"tab\t"
"quote\""
"backslash\\"
"null\0"
```

## Program Structure

```tiq
// Top-level statements form the entry point
print("Hello, world!")

// Functions defined at top level
helper x -> x * 2

// Call functions
result = helper(21)
```

## Common Patterns

```tiq
// Absolute value
abs x: i64 : i64 -> x < 0 ? 0 - x : x

// Max of two
max a: i64 b: i64 : i64 -> a > b ? a : b

// Factorial (recursive)
factorial n: i64 : i64 -> n <= 1 ? 1 : n * factorial(n - 1)

// Sum with loop
sum_to n: i64 : i64 -> {
    total <- 0
    [i <- 0..n] {
        total <- total + i
    }
    total
}
```

## CLI Commands

```sh
tiq check file.tiq       # Check syntax and types
tiq run file.tiq         # Compile and run
tiq build file.tiq -o out  # Build executable
tiq emit-c file.tiq      # Show generated C
tiq emit-ir file.tiq     # Show SSA IR
```

## Key Rules

- `=` binds immutable, `<-` binds mutable
- No `if/else` keywords — use ternary `? :`
- `skip` and `break` must be in braces: `{ skip }`
- `??` works only on `Option` and `Result` types
- Pattern matching requires wildcard `_ =>` arm
- Enum variants separated by commas
- No hex literals (use decimal)
- Top-level statements are the entry point (no `main`)
