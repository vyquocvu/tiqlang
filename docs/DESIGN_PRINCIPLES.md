# Tiq Design Principles

## Product statement

**Tiq is a tiny compiled language for fast tools and services.**

## Priorities

When goals conflict, use this order:

1. Correct and deterministic semantics
2. Fast compilation
3. Small emitted programs
4. Small, learnable language
5. Short source code
6. Peak runtime performance

Short syntax never justifies ambiguous semantics or surprising operator behavior.

## Semantic density, not code golf

Tiq removes repeated ceremony:

```tiq
add a b -> a + b
fib = [0, 1, ... a + b]
[0..10 | print(i)]
```

It does not redefine familiar operators merely to save characters. These keep conventional meanings:

```text
+ - * / % == != < <= > >= && || ! & | ^ << >>
```

## Small syntax budget

A new syntax feature must demonstrate that it:

- appears frequently enough to deserve syntax;
- cannot be expressed clearly by an ordinary function;
- parses deterministically;
- composes with existing precedence;
- has one primary meaning;
- does not require a large runtime feature.

## Explicit mutation

```tiq
x = 1       // immutable
x <- 1      // mutable declaration
x <- x + 1  // reassignment
```

Mutation must be visible at both declaration and update sites.

## Fail closed

Unsupported source is a compiler error. The compiler must never silently reinterpret unknown syntax, generate placeholder code, or downgrade an error to a warning that changes semantics.

## No invisible runtime

The base language has no mandatory:

- garbage collector;
- virtual machine;
- reflection metadata;
- exception unwinder;
- global scheduler;
- package initialization side effects.

Libraries may provide opt-in facilities when their costs are explicit.

## Tooling is part of the language

The canonical tool is `tiq`:

```text
tiq build app.tiq -o app
tiq run app.tiq
tiq check app.tiq
tiq emit-c app.tiq
tiq fmt app.tiq
tiq test
```

The compiler, formatter, diagnostics, build metadata, and standard library versions are considered part of language compatibility.

## Bootstrap strategy

The first compiler is ISO C11 and emits portable C11. This provides:

- a minimal trusted implementation;
- fast compiler builds;
- broad platform reach;
- easy inspection of generated output;
- a path to native backends later without blocking language validation.

## Non-goals for early Tiq

- object-oriented inheritance;
- dynamic typing;
- arbitrary operator overloading;
- compile-time code execution;
- hygienic macro systems;
- implicit allocation;
- implicit concurrency;
- source-level compatibility with C, Go, Rust, or Python;
- becoming a general solution for every software domain.
