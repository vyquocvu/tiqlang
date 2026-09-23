# Migrating to Tiq

A guide for developers coming from other languages. Tiq combines familiar concepts with a minimal syntax focused on semantic density.

## From C

### Functions

**C:**
```c
int add(int a, int b) {
    return a + b;
}
```

**Tiq:**
```tiq
add a: i64 b: i64 : i64 -> a + b
```

Tiq functions return their final expression. No `return` keyword needed for simple cases.

### Variables

**C:**
```c
const int x = 42;
int y = 10;
y = 20;
```

**Tiq:**
```tiq
x = 42          // Immutable by default
y <- 10         // Mutable
y <- 20         // Update
```

Tiq uses `=` for immutable bindings and `<-` for mutable ones. No `const` keyword.

### Control Flow

**C:**
```c
if (x > 0) {
    printf("positive\n");
} else {
    printf("non-positive\n");
}
```

**Tiq:**
```tiq
result = x > 0 ? "positive" : "non-positive"
print(result)
```

Tiq has no `if/else` statements. Use the ternary operator `? :` for conditional expressions.

### Loops

**C:**
```c
for (int i = 0; i < 10; i++) {
    printf("%d\n", i);
}
```

**Tiq:**
```tiq
[i <- 0..10] {
    print(i)
}
```

Tiq uses bracket loops `[var <- range]` for iteration. Ranges are half-open: `0..10` means 0 to 9.

### Structs

**C:**
```c
struct Point {
    int x;
    int y;
};

struct Point p = {10, 20};
printf("%d\n", p.x);
```

**Tiq:**
```tiq
struct Point {
    x: i64,
    y: i64
}

p = Point { x: 10, y: 20 }
print(p.x)
```

### Memory Management

**C:**
```c
int* ptr = malloc(sizeof(int));
*ptr = 42;
free(ptr);
```

**Tiq:**
```tiq
// Tiq uses ownership and borrowing
// No manual malloc/free in safe code
value = 42
```

Tiq has a linear type system with ownership and borrows. No garbage collector, no manual memory management in safe code.

## From Rust

### Functions

**Rust:**
```rust
fn add(a: i64, b: i64) -> i64 {
    a + b
}
```

**Tiq:**
```tiq
add a: i64 b: i64 : i64 -> a + b
```

Tiq syntax is more compact. No `fn` keyword, no curly braces for single-expression functions.

### Option and Result

**Rust:**
```rust
let opt: Option<i32> = Some(42);
let value = opt.unwrap_or(0);

let res: Result<i32, &str> = Ok(100);
let value = res.unwrap_or(0);
```

**Tiq:**
```tiq
opt_val = some(42)
value = opt_val ?? 0  // Fallback operator

res = ok(100)
value = res ?? 0
```

Tiq uses `??` for fallback on both `Option` and `Result` types.

### Pattern Matching

**Rust:**
```rust
match value {
    0 => println!("zero"),
    1 => println!("one"),
    _ => println!("other"),
}
```

**Tiq:**
```tiq
match value {
    0 => print("zero"),
    1 => print("one"),
    _ => print("other")  // Wildcard required
}
```

Tiq pattern matching requires a wildcard `_ =>` arm to ensure exhaustiveness.

### Ownership

**Rust:**
```rust
let s1 = String::from("hello");
let s2 = s1;  // s1 is moved
// println!("{}", s1);  // Error: s1 is moved
```

**Tiq:**
```tiq
s1 = "hello"
s2 = s1  // s1 is moved
// print(s1)  // Error: s1 is moved
```

Tiq has similar ownership semantics but with a simpler syntax.

## From Python

### Functions

**Python:**
```python
def add(a, b):
    return a + b
```

**Tiq:**
```tiq
add a b -> a + b
```

Tiq can infer types. No `def` keyword, no `return` for single-expression functions.

### Variables

**Python:**
```python
x = 42
y = [1, 2, 3]
```

**Tiq:**
```tiq
x = 42
y = [1, 2, 3]
```

Similar syntax, but Tiq is statically typed and compiled.

### Loops

**Python:**
```python
for i in range(10):
    print(i)
```

**Tiq:**
```tiq
[i <- 0..10] {
    print(i)
}
```

Tiq uses bracket loops instead of `for` statements.

### Classes and Structs

**Python:**
```python
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

p = Point(10, 20)
print(p.x)
```

**Tiq:**
```tiq
struct Point {
    x: i64,
    y: i64
}

p = Point { x: 10, y: 20 }
print(p.x)
```

Tiq uses structs for data, not classes. No methods on structs (yet).

## From Go

### Functions

**Go:**
```go
func add(a int, b int) int {
    return a + b
}
```

**Tiq:**
```tiq
add a: i64 b: i64 : i64 -> a + b
```

Tiq syntax is more compact. No `func` keyword.

### Error Handling

**Go:**
```go
result, err := doSomething()
if err != nil {
    log.Fatal(err)
}
```

**Tiq:**
```tiq
result = do_something() ?? 0  // Fallback on error
```

Tiq uses the `??` operator for error handling with `Result` types.

### Goroutines and Concurrency

**Go:**
```go
go func() {
    fmt.Println("async")
}()
```

**Tiq:**
```tiq
// Tiq concurrency is still evolving
// See docs/POST_BOOTSTRAP_ROADMAP.md
```

Tiq's concurrency model is under development. See the roadmap for details.

## Key Differences

### No Implicit Behavior

Tiq avoids implicit behavior:
- No implicit type conversions
- No implicit memory allocation
- No garbage collector
- No hidden control flow

### Expression-Oriented

Most Tiq constructs are expressions that return values:
- Functions return their final expression
- Ternary is an expression, not a statement
- Blocks return their final expression

### Minimal Syntax

Tiq has fewer keywords and special forms:
- No `if/else` (use ternary)
- No `for/while` (use bracket loops)
- No `return` for simple functions
- No `class` (use `struct`)

### Deterministic Costs

Tiq makes costs visible:
- No hidden allocations
- No reflection runtime
- No exception unwinder
- No hidden scheduler

## Next Steps

- Read [The Tiq Book](TIQ_BOOK.md) for a tutorial introduction
- Check [Tiq by Example](TIQ_BY_EXAMPLE.md) for common patterns
- Browse the [Quick Reference](QUICK_REFERENCE.md) for syntax details
- Try the [examples](../examples/) to see real programs
