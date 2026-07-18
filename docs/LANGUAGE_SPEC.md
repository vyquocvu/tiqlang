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

Reserved words in v0.1:

```text
true false while for in break continue
```

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
=    immutable definition or function definition
:=   mutable definition
<-   reassignment
? :  conditional expression
..   half-open range
=>   lambda or match arm; reserved for a later v0.x
??   optional fallback; reserved for a later v0.x
```

`!` is logical negation inside an expression. At statement start, `!expr` is the print statement. This distinction is syntactic and unambiguous.

## 6. Bindings

Immutable definition:

```tiq
port = 8080
```

Mutable definition and reassignment:

```tiq
count := 0
count <- count + 1
count += 1
```

Reading an uninitialized binding, redefining a name in the same scope, or assigning to an immutable binding is a compile-time error.

## 7. Functions

Single-expression function:

```tiq
add a b = a + b
```

Typed form planned for v0.2:

```tiq
add a:i32 b:i32 -> i32 = a + b
```

In v0.1, parameter and return types are inferred from use where possible. A program whose public or recursive function type cannot be inferred is rejected.

The value of the final expression is the function result.

```tiq
max a b = a > b ? a : b
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
clamp x lo hi = {
  x < lo ? lo : x > hi ? hi : x
}
```

A block evaluates to its final expression. Statements before the final expression end with a newline or `;`.

## 10. Loops

While loop:

```tiq
while count < 10 {
  count += 1
}
```

Range loop:

```tiq
for i in 0..10 {
  !i
}
```

Ranges are half-open: `a..b` contains `a` through `b-1`.

## 11. Print statement

```tiq
!"Hello"
!value
```

The bootstrap implementation supports strings first. Later implementations dispatch through the standard formatting protocol.

## 12. Primitive types

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

Integer literals default to the smallest compatible signed type, with `i64` as the fallback. Floating literals default to `f64`.

## 13. Arrays and slices

Planned v0.2 syntax:

```tiq
xs = [1,2,3]
first = xs[0]
part = xs[1..3]
```

Array bounds are checked unless the compiler proves the access safe. An explicit unsafe facility is deferred.

## 14. Errors

Tiq has no exceptions. Fallible functions return a result value. The concrete result and propagation syntax will be standardized after the core type checker exists. Until then, the compiler must not invent implicit failure behavior.

## 15. Memory

Values have deterministic scope-based destruction. Scalars and small aggregates use value semantics. Heap allocation, ownership transfer, borrowing, and shared ownership are specified in `MEMORY_MODEL.md` and are not part of the bootstrap slice.

## 16. Program entry

Top-level executable statements form the implicit entry point. Libraries may contain definitions only. A future explicit `main` function may be supported but is not required for scripts and tools.

## 17. Bootstrap conformance

The first compiler slice is conforming only for:

```ebnf
program         = { print_statement } ;
print_statement = "!" string_literal [ newline ] ;
```

It must reject all unsupported syntax with a non-zero exit code rather than silently generating incorrect code.
