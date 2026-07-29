# Tiq Type System

Status: substantially implemented. The bootstrap slice checks `bool`/`i64`/`f64`/`str`, sized primitives (`i8`–`u64`, `f32`), arrays, slices, string views, structs, Option/Result, and stream generators with local inference, unification-based diagnostics, explicit numeric conversions, type annotations, and composite types (M12.1–M12.7 complete; M8 Option/Result/propagation done).

## Goals

- static and sound;
- local inference where predictable;
- no runtime type tags in ordinary code;
- explicit conversions;
- useful diagnostics without whole-program guessing;
- compile-time rejection of unsupported or ambiguous programs.

## Primitive types

```text
bool
u8 u16 u32 u64
i8 i16 i32 i64
f32 f64
str
unit
never
```

`str` is an immutable UTF-8 view represented by pointer and byte length. It is not NUL-terminated by language contract.

Bootstrap deviation: the current C backend represents `str` as a NUL-terminated `const char *` and uses `strlen()`; only slices and string views (`TiqSlice`) honor the pointer+length model. The committed end state is the pointer+length model — `str` becomes a view over immutable bytes, `len()` reads a stored length, and NUL termination becomes an FFI boundary concern only. The backend migration is scheduled with the M12 type-system work.

## Literal inference

- integer literals default to `i64` unless the context requires another integer type; literals that do not fit the resolved type are rejected at compile time;
- decimal literals default to `f64`;
- string literals have type `str`;
- `true` and `false` have type `bool`.

A literal may be constrained by its destination or function parameter.

## No implicit numeric narrowing

Widening may be accepted only when value preservation is guaranteed. Narrowing, signedness changes, and floating/integer conversion require an explicit conversion function.

```tiq
small = i8(value)
ratio = f64(count) / f64(total)
```

Implemented (M12.3): `i8(x)`, `i16(x)`, `i32(x)`, `i64(x)`, `u8(x)`, `u16(x)`, `u32(x)`, `u64(x)`, `f32(x)`, `f64(x)` are checked conversions between numeric types. Conversions between `bool`/`str` and numeric types are rejected (E10). Width mixing in binary operations without explicit conversion is rejected (E09).

## Bindings

`=` creates an immutable binding. `<-` creates a mutable binding. Mutability is not part of the value's type and does not transfer through assignment.

## Functions

Functions have fixed parameter and return types after inference. They are not dynamically overloaded. Recursive and exported functions will require an inferable or explicit signature.

Planned explicit syntax:

```tiq
add a:i32 b:i32 -> i32 -> a + b
```

## Conditional typing

The condition of `?:` must be `bool`. Both branches must unify to one type. There is no truthiness conversion.

Unary `!` is logical negation: its operand must be `bool` and its type is `bool` (LANGUAGE_SPEC §5). Printing is the `print` builtin (LANGUAGE_SPEC §12); it accepts one argument of a printable type and returns `int`.

## Composite types

Implemented:

```text
[T; N]   fixed array
[]T      slice
struct Name { field: T, ... }   record (nominal)
T?       optional (some(x), none, ?? fallback, ? propagation)
T!E      result (ok(x), err(e), ?? fallback, ? propagation)
```

Struct definitions use `struct Name { field: type, ... }` syntax. Record literals use `Name { field: value, ... }`. Field access uses `expr.field`.

## Generics

Generics are deferred. The initial standard library will use compiler-known primitives and concrete implementations. A future generic design must avoid mandatory runtime dictionaries and uncontrolled code-size growth.

## Type identity

Named types are nominal. Anonymous records and tuples, if added, are structural only within their explicitly defined category.

## Unsafe operations

Unsafe memory operations are not part of the default language. A future `unsafe` module may expose pointers and unchecked operations with visible scopes and no implicit promotion into safe code.
