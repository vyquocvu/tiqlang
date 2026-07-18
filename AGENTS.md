# AGENTS.md

## Mission

Build Tiq as a tiny, deterministic compiled language for fast tools and services. The bootstrap compiler is C11 and the reference backend emits C11.

## Read first

Before changing code or syntax, read:

1. `README.md`
2. `docs/DESIGN_PRINCIPLES.md`
3. `docs/LANGUAGE_SPEC.md`
4. `docs/GRAMMAR.md`
5. `docs/ROADMAP.md`
6. `docs/IMPLEMENTATION_STATUS.md`

## Rules

- Preserve familiar meanings of arithmetic, logical, comparison, and bitwise operators.
- Do not add syntax without updating the normative spec and grammar.
- Unsupported input must fail closed.
- Do not add a VM, mandatory GC, reflection runtime, exception unwinder, or hidden scheduler.
- Keep the compiler buildable with ISO C11 plus documented platform APIs.
- Avoid speculative abstractions and placeholders presented as completed features.
- Every behavior change requires a deterministic failing test first.
- Diagnostics must include source location and must be tested.
- Update implementation status and roadmap evidence with each completed package.

## Required checks

```sh
make clean
make
make test
```

Use sanitizers during parser and memory work when available:

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined'
make test
```

## Change boundaries

A language feature is incomplete unless lexer, parser, semantic checks, backend behavior, diagnostics, tests, specification, and implementation status agree.
