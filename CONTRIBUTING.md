# Contributing to Tiq

Thank you for your interest in contributing to Tiq! This document explains how to contribute effectively.

## Getting started

1. **Fork and clone** the repository
2. **Build the compiler**: `make`
3. **Run the test suite**: `make test`
4. **Read the docs**: Start with [DESIGN_PRINCIPLES.md](docs/DESIGN_PRINCIPLES.md), [LANGUAGE_SPEC.md](docs/LANGUAGE_SPEC.md), and [GRAMMAR.md](docs/GRAMMAR.md)

## How to contribute

### Bug reports

- Check existing issues first
- Include a minimal reproducible example
- Specify your platform (macOS/Linux), compiler version (`clang --version` or `gcc --version`), and Tiq commit hash
- Include the full error message and diagnostic output

### Feature requests

- Open an issue describing the problem you're trying to solve
- Explain why the feature fits Tiq's design philosophy (semantic density, explicit intent, no hidden costs)
- Be prepared to discuss tradeoffs

### Code contributions

**Before writing code:**

1. Open an issue or comment on an existing one to discuss your approach
2. Read [AGENTS.md](AGENTS.md) — it defines the compiler's invariants and required checks
3. Understand the change boundaries: a language feature is incomplete unless lexer, parser, semantic checks, backend behavior, diagnostics, tests, specification, and implementation status all agree

**Development workflow:**

```sh
# Build with sanitizers during parser/memory work
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined'
make test

# Clean build before submitting
make clean
make
make test
```

**Required checks before submitting:**

- [ ] `make clean && make && make test` passes
- [ ] New behavior has a deterministic failing test first (TDD)
- [ ] Diagnostics include source location
- [ ] Specification and grammar docs are updated if syntax or semantics changed
- [ ] Implementation status is updated if a milestone progressed

**What we merge:**

- Bug fixes with regression tests
- Documentation improvements (clarity, accuracy, examples)
- Test coverage for untested behavior
- Performance improvements with benchmarks showing no regressions
- Small, focused features that align with the roadmap

**What we don't merge:**

- Speculative abstractions or placeholders presented as completed features
- Syntax changes without spec/grammar/test/backend alignment
- Features that add mandatory GC, reflection, exception unwinding, or hidden scheduling
- Changes that break the ISO C11 build requirement

### Documentation

Documentation improvements are always welcome:

- Fix typos, clarify explanations, add examples
- Ensure code examples compile with `tiq check`
- Update cross-references when moving sections
- Keep the tone practical and honest about limitations

## Code style

- C11 code follows the existing style in `src/` (see existing files for conventions)
- Tiq code follows the formatter rules in `docs/CLI.md` §Formatter
- Comments explain **why**, not **what** — well-named identifiers explain the what
- No speculative TODOs or placeholders

## Testing

- Unit tests live in `tests/` as shell scripts
- Each test script is self-contained and prints `pass` or `fail`
- Run the full suite with `make test`
- Add tests for new behavior before implementing it

## Questions?

- Open a GitHub issue for discussion
- Check existing docs before asking — the answer may already be there

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
