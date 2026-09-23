# Tiq by Example

A pattern-based reference for common Tiq idioms. Each example is a complete, runnable program.

## Table of Contents

- [Hello World](#hello-world)
- [Variables and Types](#variables-and-types)
- [Functions](#functions)
- [Control Flow](#control-flow)
- [Recurrences](#recurrences)
- [Loops](#loops)
- [Structs](#structs)
- [Pattern Matching](#pattern-matching)
- [Option and Result](#option-and-result)
- [Error Handling](#error-handling)

## Hello World

```tiq
print("Hello, world!")
```

Run: `tiq run hello.tiq`

## Variables and Types

### Immutable bindings

```tiq
x = 42
name = "Tiq"
pi = 3.14159
flag = true

print(x)
print(name)
```

Use `=` for immutable bindings. The type is inferred from the value.

### Mutable bindings

```tiq
count <- 0
count <- count + 1
count <- count + 1

print(count)  // 2
```

Use `<-` for mutable bindings. You can update them with `<-`.

### Explicit types

```tiq
add a: i32 b: i32 : i32 -> a + b

print(add(10, 20))  // 30
```

Use `:` to declare parameter and return types. Types can also be inferred.

## Functions

### Expression functions

```tiq
square n -> n * n
max a b -> a > b ? a : b

print(square(7))   // 49
print(max(10, 20)) // 20
```

Functions return their final expression. No `return` keyword needed.

### Block functions

```tiq
factorial n: i64 : i64 -> {
    n <= 1 ? 1 : n * factorial(n - 1)
}

print(factorial(5))  // 120
```

Use `{ }` for multi-statement function bodies.

### Multiple parameters

```tiq
add a: i64 b: i64 : i64 -> a + b
distance x1: f64 y1: f64 x2: f64 y2: f64 : f64 -> {
    dx <- x2 - x1
    dy <- y2 - y1
    (dx * dx + dy * dy)
}

print(add(10, 20))
```

Parameters are separated by spaces, not commas.

## Control Flow

### Ternary operator

```tiq
x = 10
result = x > 0 ? "positive" : "non-positive"

print(result)  // positive
```

Use `condition ? then : else` for conditional expressions.

### Skip and break

```tiq
[i <- 1..10] {
    i % 2 == 0 ? { skip }  // Skip even numbers
    print(i)
}
// Prints: 1 3 5 7 9

[count <- 0..100] {
    count * count > 50 ? { break }
    print(count * count)
}
// Prints: 0 1 4 9 16 25 36 49
```

Use `skip` to continue to the next iteration, `break` to exit the loop. Must be in braces within ternary.

## Recurrences

### Fibonacci sequence

```tiq
fib = [0, 1, ... (a, b) -> a + b]

print(fib[0])   // 0
print(fib[1])   // 1
print(fib[10])  // 55
```

Recurrences have seeds and a rule. `(a, b)` names the two preceding terms.

### Factorial as recurrence

```tiq
factorial = [1, ... (x; i) -> x * i]

print(factorial[0])  // 1
print(factorial[1])  // 1
print(factorial[5])  // 120
```

Use `(x; i)` to name the preceding term `x` and the index `i`.

### Parameterized recurrences

```tiq
powers base -> [1, ... (x) -> x * base]

p2 = powers(2)
p3 = powers(3)

print(p2[0])   // 1
print(p2[10])  // 1024
print(p3[4])   // 81
```

Recurrences can be functions that return sequences.

## Loops

### Range loops

```tiq
[i <- 0..5] {
    print(i)
}
// Prints: 0 1 2 3 4
```

Use `[var <- start..end]` for range loops. The end is exclusive.

### Loops with step

```tiq
[i <- 0..10] {
    i % 2 == 0 ? { print(i) }
}
// Prints: 0 2 4 6 8
```

Filter with conditional logic inside the loop body.

## Structs

### Define and use structs

```tiq
struct Point {
    x: i64
    y: i64
}

origin = Point { x: 0, y: 0 }
p = Point { x: 10, y: 20 }

print(p.x)  // 10
print(p.y)  // 20
```

Define structs with `struct Name { fields }`. Create instances with `Name { field: value }`.

### Struct methods

```tiq
struct Rectangle {
    width: i64
    height: i64
}

area r: Rectangle : i64 -> r.width * r.height

rect = Rectangle { width: 10, height: 20 }
print(area(rect))  // 200
```

Define functions that take struct parameters as "methods."

## Pattern Matching

### Match on values

```tiq
classify n: i64 : str ->
    match n {
        0 => "zero",
        1 => "one",
        _ => "many"
    }

print(classify(0))   // zero
print(classify(1))   // one
print(classify(42))  // many
```

Use `match` with arms. The wildcard `_` catches everything else.

### Match on enums

```tiq
enum Color {
    Red,
    Green,
    Blue
}

describe c: i64 : str ->
    match c {
        Color.Red => "red",
        Color.Green => "green",
        Color.Blue => "blue",
        _ => "unknown"
    }

print(describe(Color.Red))  // red
```

Match on enum variants using `EnumName.VariantName`.

## Option and Result

### Option: some or none

```tiq
find_first_even arr: [i64] : option i64 -> {
    [x <- arr] {
        x % 2 == 0 ? { x }
    }
    none
}

nums = [1, 3, 4, 7]
result = find_first_even(nums)

print(result ?? -1)  // 4
```

Use `option T` for values that might be absent. Use `some(value)` or `none`.

### Result: ok or err

```tiq
divide a: i64 b: i64 : result i64 str ->
    b == 0 ? err("division by zero") : ok(a / b)

r1 = divide(10, 2)
r2 = divide(10, 0)

print(r1 ?? -1)  // 5
print(r2 ?? -1)  // -1
```

Use `result T E` for operations that can fail. Use `ok(value)` or `err(error)`.

### Match on Option/Result

```tiq
opt = some(42)

msg = match opt {
    some(val) => "found value",
    none => "nothing here",
    _ => "unreachable"
}

print(msg)  // found value
```

Use pattern matching to destructure Option and Result.

## Error Handling

### Fallback operator

```tiq
opt = none
value = opt ?? 0

print(value)  // 0
```

Use `??` to provide a fallback for Option/Result.

### Propagate errors

```tiq
parse_and_double s: str : result i64 str -> {
    n <- parse_int(s) ?? err("not a number")
    ok(n * 2)
}

r1 = parse_and_double("21")
r2 = parse_and_double("abc")

print(r1 ?? -1)  // 42
print(r2 ?? -1)  // -1
```

Use `??` to early-return on error. The function must return a Result.

---

See [The Tiq Book](TIQ_BOOK.md) for a comprehensive tutorial, or browse [examples/](../examples/) for more real-world programs.
