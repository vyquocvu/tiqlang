# Tiq Language Specification v0.1

Status: Draft, normative for the bootstrap compiler.

## 1. Scope

Tiq is a statically typed, ahead-of-time compiled language for small command-line tools and network services. Version 0.1 deliberately defines a small core. Features not described here are unsupported.

## 2. Source files

- Extension: `.tiq`
- Encoding: UTF-8; v0.1 identifiers are ASCII only.
- Newlines separate statements unless grouping is open.
- `//` starts a line comment.
- Top-level statements execute in source order inside an implicit program entry point.

## 3. Design constraints

1. Familiar arithmetic, comparison, logical, and bitwise operators keep their conventional meanings.
2. The language removes ceremony rather than replacing ordinary programming with code-golf symbols.
3. Parsing must be deterministic without semantic backtracking.
4. Allocation and control flow must be visible in source or derivable without runtime reflection.
5. No mandatory VM, garbage collector, exception runtime, or global async runtime.

## 4. Lexical elements

Identifiers match `[A-Za-z_][A-Za-z0-9_]*`.

Reserved words in v0.1 (matching the lexer exactly):

```text
break chan defer false match move mut skip spawn struct true until while
```

`while` and `until` are clause keywords only: they appear in stream generator bounds and predicate slicing (§14). There are no `while`/`for` statement forms (§10). `chan`, `spawn`, `match`, `struct`, and `mut` are reserved for provisional or rejected constructs (§17).

Literals:

```tiq
42
-7
3.14
true
false
"hello"
```

Escape sequences: `\\`, `\"`, `\n`, `\r`, `\t`, and `\0`.

## 5. Operators

Tiq preserves conventional operators:

```text
+ - * / %
== != < <= > >=
&& || !
& | ^ << >>
+= -= *= /= %=
```

Tiq-specific operators:

```text
=    immutable definition
<-   mutable definition or reassignment
->   function definition
? :  conditional expression
..   half-open range
...  stream generator expansion
=>   match arm (§17.1); lambda reserved for a later v0.x
??   optional fallback; reserved for a later v0.x
```

`!` is logical negation. Its operand must be `bool` and the result is `bool`; Tiq has no truthiness. Printing is not an operator — it is the `print` builtin (§12).

## 6. Bindings

Immutable definition:

```tiq
port = 8080
```

Mutable definition and reassignment:

```tiq
count <- 0
count <- count + 1
count += 1
```

Reading an uninitialized binding, redefining a name in the same scope, or assigning to an immutable binding is a compile-time error.

## 7. Functions

Single-expression function:

```tiq
add a b -> a + b
```

Typed form planned for v0.2:

```tiq
add a:i32 b:i32 -> i32 -> a + b
```

In v0.1, parameter and return types are inferred from use where possible. A program whose public or recursive function type cannot be inferred is rejected.

The value of the final expression is the function result.

```tiq
max a b -> a > b ? a : b
```

A function body may also be a block (§9); the block's final expression is the result:

```tiq
classify n -> {
  n < 0 ? "neg" : n > 0 ? "pos" : "zero"
}
```

Recursive functions are allowed after their complete signature can be inferred.

## 8. Conditional expressions

```tiq
label = age >= 18 ? "adult" : "minor"
```

The condition must be `bool`. Both branches must have a compatible type.

## 9. Blocks

Indentation is not semantic in v0.1. Multi-statement blocks use braces:

```tiq
clamp x lo hi -> {
  x < lo ? lo : x > hi ? hi : x
}
```

A block evaluates to its final expression. Statements before the final expression end with a newline or `;`.

## 10. Loops

Tiq uses unified **Bracket Loops (`[domain] { body }`)** for iteration; there are no `for` or `while` statement forms. The loop header in `[ ]` takes a full expression; the body is a `{ }` block whose statements are separated by newlines or `;`.

Range iteration:

```tiq
[0..10] { print(i) }
```

Range loops bind an implicit index `i`. An optional binder names the loop variable instead (the binder replaces `i`):

```tiq
[j <- 0..10] { print(j) }
```

Loop variables are immutable inside the body; assigning to them is an error (E11). Binders are only valid for range domains; `[j <- condition]` is rejected with `loop binder requires a range domain` (E15).

Multiple binders iterate the Cartesian product as nested loops; later binders may reference earlier ones, `break`/`skip` apply to the innermost loop, and duplicate binder names are rejected (`duplicate loop binder`, E15):

```tiq
[j <- 0..3, k <- 0..j] { print(j * 10 + k) }
```

Conditional iteration:

```tiq
[count < 10] { count += 1 }
```

Loop control statements:
- `break`: Terminate loop execution immediately (`[0..10] { print(i); break }`).
- `skip`: Skip the remainder of the current iteration (`[1..4] { print(x); skip; x += 100 }`).

Inline guards (`break if condition`, `skip if condition`) are planned but not implemented; the bootstrap compiler rejects them (fail closed).

## 11. Primitive types

Planned core primitive types:

```text
bool
u8 u16 u32 u64
 i8 i16 i32 i64
f32 f64
str
unit
never
```

Integer literals default to `i64` unless the context requires another integer type. Floating literals default to `f64`. Integer literals that do not fit the resolved type are rejected at compile time.

## 12. The `print` builtin

`print(expression)` writes the expression's value and a trailing newline to standard output. It is an ordinary builtin function call, not dedicated syntax, and takes exactly one argument:

```tiq
print(42)          // prints 42
print("hello")     // prints hello
print(fib[10])     // prints 55
```

Output formatting is determined by the argument's static type: integers print in decimal, floats with shortest-round-trip style (`%g`), `bool` as `true`/`false`, `str` and string/array views as their bytes. Arguments whose type has no printable representation (arrays, streams, functions) are rejected at compile time.

`print` returns the number of bytes written as `int`. Historical note: earlier drafts overloaded statement-position `!` as a print statement; that form was removed — `!` is logical negation only (§5).

## 13. Arrays and Slices

Array literals are written with square brackets and comma-separated elements:

```tiq
xs = [1, 2, 3]
```

All elements must have the same type. The element type is inferred from the literal. Array access uses indexing syntax:

```tiq
first = xs[0]
```

The index must be an `int` value. Multi-dimensional arrays are not supported in v0.1.

An array fill `[value; length]` constructs an array whose elements are all `value`:

```tiq
zeros = [0; 4]   // [0, 0, 0, 0]
```

In v0.1 the fill length must be an integer literal.

Array bounds are checked unless the compiler proves the access safe. An explicit unsafe facility is deferred.

### 13.1 Slices and String Views

Slices construct non-owning dynamic views over contiguous sequence elements (array elements or string characters). Slicing syntax uses half-open range notation inside index brackets:

```tiq
sub = xs[1..3]   // slice of xs from index 1 (inclusive) to 3 (exclusive)
tail = xs[1..]   // slice from index 1 to end
head = xs[..2]   // slice from start (0) to index 2
full = xs[..]    // full slice view of all elements
```

Slicing a string `s[i..j]` returns a `str` view over the designated byte range.

Indexing a string or string view with a single index `s[i]` returns the byte at position `i` as an integer:

```tiq
s = "abc"
print(s[0])   // prints 97
```

Omitting the start index defaults to `0`. Omitting the end index defaults to `len(xs)` (or string length for `str`).

Slice bounds are evaluated at runtime. Slicing dynamically enforces 0 <= start <= end <= len. If a slice bound is violated, execution halts deterministically.

The built-in `len()` returns the runtime length of arrays, array slices, and string views:

```tiq
print(len(xs))       // prints 3
print(len(xs[1..3])) // prints 2
```

`len()` accepts seeds of exactly one argument, which must be an array, slice, or string view.

## 14. Stream Generators

Stream generators define infinite or lazy recursive sequences via initial seed values followed by `...` and a windowed combination expression:

```tiq
fib = [0, 1, ... a + b]
first = fib[0]   // 0
tenth = fib[10]  // 55

// Bounded stream generator
powers = [1, ... x * 2 while x < 100]

// Predicate slicing
print(fib[while x < 100])
```

The number of seed elements determines the window size bound to preceding terms in the generator expression (e.g. `a + b` binds the previous two terms). Generators can specify inline termination bounds (`while condition` or `until condition`). Indexing or predicate slicing evaluates or retrieves terms in $O(k)$ time using $O(1)$ state.

The generator expression evaluates in a context that binds exactly these names:

- `a` — the previous term, and `b` — the term before it (generators with two or more seeds);
- `x` — the previous term (single-seed generators);
- `i` — the zero-based index of the term being computed (always available).

No other context names exist. v0.1 windows are limited to the two preceding terms even when more than two seeds are given.

## 15. Errors

Tiq has no exceptions. Fallible functions return a result value.

### 15.1 Design sketch (not implemented)

The following direction is normative for design work but has no implementation; the compiler rejects all of this syntax until it lands (fail closed, targeted at M12.6):

- `T?` and `T!E` are postfix **type constructors**, not runtime operators: `T?` is an optional (`T` or absent), `T!E` is a result (`T` or an error of type `E`).
- `??` is the fallback operator: `r = fs_read("f") ?? ""`. It short-circuits — the right operand is evaluated only when the left operand is absent or an error. Its precedence sits between `||` and `?:` (tighter than `?:`, looser than `||`), so `a ?? b ? x : y` parses as `(a ?? b) ? x : y`.
- `expr?` is **expression-level** propagation: if `expr` is absent or an error, the enclosing function returns that state immediately; otherwise `expr?` evaluates to the unwrapped value. The enclosing function's return type must itself be optional/result-compatible, checked at compile time.
- A `T!E` value destructures with `match`; no dedicated destructuring syntax is planned.

Until then, the compiler must not invent implicit failure behavior.

## 16. Memory

Values have deterministic scope-based destruction. Scalars and small aggregates use value semantics.

### 16.1 Move semantics

The `move` keyword transfers ownership of a binding to a new location:

```tiq
x <- [1, 2, 3]
y <- move x   // ownership transferred to y
```

After a move, the source binding is invalidated. Any subsequent use of the source produces a compile-time error. Moving an immutable binding (`=`) is also a compile-time error.

Compound assignment resets the moved state, allowing the binding to be reused:

```tiq
x <- [1, 2, 3]
y <- move x
x += 1   // x is valid again with a new value
```

Heap allocation, borrowing, and shared ownership are specified in `MEMORY_MODEL.md` and are not part of the bootstrap slice.

### 16.2 Deferred actions

The `defer` keyword schedules an expression to run when the enclosing block exits:

```tiq
{
    print(1)
    defer print(2)
    print(3)
}
// Output:
// 1
// 3
// 2
```

Deferred actions execute in reverse declaration order. `defer` is only valid inside `{ }` blocks. A `defer` outside a block is a compile-time error.

## 17. Provisional constructs

The bootstrap parser accepts a few constructs ahead of their full specification. Their semantics are partial, they are explicitly provisional, and they may change or be removed.

### 17.1 Match expressions (provisional)

`match` selects the first arm whose pattern compares equal to the scrutinee:

```tiq
x = 10
res = match x { 10 => 100, 20 => 200 }
```

Patterns are equality-compared expressions; there are no binding or destructuring patterns in v0.1. All arm bodies must have the same type. Exhaustiveness checking is deferred; in the bootstrap backend an unmatched scrutinee yields `0`.

### 17.2 Field access (provisional)

`expr.field` parses and type-checks against struct types, but no surface syntax constructs a struct type yet: `struct` definitions and record literals do not parse and fail closed (pending M12.4 spec-and-grammar-first syntax).

### 17.3 Reserved and rejected

`chan expr`, `spawn expr`, and borrow prefixes (`&x`, `&mut x`) parse but are rejected during semantic analysis with a "not supported yet" diagnostic (fail closed). `mut` is reserved for borrow syntax only.

## 18. Program entry

Top-level executable statements form the implicit entry point. Libraries may contain definitions only. A future explicit `main` function may be supported but is not required for scripts and tools.

## 19. Bootstrap conformance

The bootstrap compiler must reject all unsupported syntax with a non-zero exit code rather than silently generating incorrect code.
