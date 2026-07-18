# Tiq Type System

Status: design target; only string print programs are implemented by the bootstrap slice.

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

## Literal inference

- integer literals choose the smallest compatible signed type, falling back to `i64`;
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

## Bindings

`=` creates an immutable binding. `:=` creates a mutable binding. Mutability is not part of the value's type and does not transfer through assignment.

## Functions

Functions have fixed parameter and return types after inference. They are not dynamically overloaded. Recursive and exported functions will require an inferable or explicit signature.

Planned explicit syntax:

```tiq
add a:i32 b:i32 -> i32 = a + b
```

## Conditional typing

The condition of `?:` must be `bool`. Both branches must unify to one type. There is no truthiness conversion.

## Composite types

Planned:

```text
[T; N]   fixed array
[]T      slice
{...}    record
T?       optional
T!E      result
```

Exact surface syntax remains provisional until parser and type-checker milestones are complete.

## Generics

Generics are deferred. The initial standard library will use compiler-known primitives and concrete implementations. A future generic design must avoid mandatory runtime dictionaries and uncontrolled code-size growth.

## Type identity

Named types are nominal. Anonymous records and tuples, if added, are structural only within their explicitly defined category.

## Unsafe operations

Unsafe memory operations are not part of the default language. A future `unsafe` module may expose pointers and unchecked operations with visible scopes and no implicit promotion into safe code.
