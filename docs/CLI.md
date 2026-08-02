# Tiq CLI

Canonical executable: `tiq`.

## Implemented commands

The following commands match the `usage:` output of `tiq` exactly:

```text
tiq --version
tiq run <file.tiq>
tiq build <file.tiq> [-o output] [--target <triple>]
tiq emit-c <file.tiq>
tiq check <file.tiq>...
```

### Debug / inspect commands

These commands emit intermediate representations for debugging and are not part of the primary workflow:

```text
tiq dump-tokens <file.tiq>
tiq dump-ast <file.tiq>
tiq dump-typed-ast <file.tiq>
```

### Developer tooling (Tiq programs, M14)

Developer tooling is written in Tiq and built from `src/tiq/tools/` by the bootstrap (`make tool-test`, `tool-fmt`, `tool-bench`, `tool-init` build them into `build/`). The `tiq <tool>` subcommand names below are the interface each tool implements; the C bootstrap binary dispatches them by building and running the corresponding Tiq program.

```text
tiq test [--verbose] [--list] [--tiq <compiler>] [dir|file...]
tiq fmt [--check] [--output <file>] [--use-tabs] [--indent-width <n>] [file]
tiq bench [-i N] <file|dir>...
tiq init [name]
tiq init --check <file.tiq.toml>
tiq cache clear
tiq cache <path>
```

- `tiq test` discovers `.tiq` files (non-recursively, hidden names skipped), extracts the expected stdout from `//! expected:` marker lines (LANGUAGE_SPEC §2.1), builds and runs each test with the given compiler, and reports a `Tests: N passed, M failed, K skipped` summary on stdout. Failures and surfaced compiler diagnostics go to stderr. Exit code is 1 iff any test failed.
- `--tiq <compiler>` selects the compiler binary used to build tests (defaults to `tiq` on `PATH`).
- `--list` prints each discovered test path without running anything.
- Files without a `//! expected:` marker are skipped. Transient build artifacts are created next to each test file and removed afterwards, so paths must not contain spaces or single quotes.

- `tiq fmt` is a deterministic, token-based formatter (built on the self-hosted lexer; no parsing, so no semantic checking). It reads `file`, or standard input when no file is given, and writes the formatted source to standard output, or to `--output <file>` when given. Stdin is read via `/dev/stdin` and therefore requires a seekable source (a file redirect, not a pipe).
- `--check` compares the input to the formatted output without writing anything; it exits 0 when they are byte-identical and prints `<file>: not formatted` to stderr and exits 1 otherwise. `--check` requires a file argument and cannot be combined with `--output`.
- `--use-tabs` indents with tabs; otherwise `--indent-width <n>` (default 4) spaces per level are used. `--indent-width` must be `>= 1`.
- A lexical error in the input fails closed: the formatter exits 1 with the located diagnostic on stderr. `--output` and stdin failures also exit 1; unknown options, a missing `--indent-width`/`--output` argument, or an invalid width exit 2.

- `tiq bench` measures the self-hosted compiler's per-phase time (lexer, parser, semantic checker) for each target — a named `.tiq` file or every `.tiq` file in a directory (non-recursive, hidden names skipped, sorted order). Each file is flattened (`mod_flatten`, LANGUAGE_SPEC §3.4) exactly as the real driver does, then each phase is timed with the monotonic `clock_ms` builtin (§19.6) over `-i N` iterations (default 1) of fresh driver state. It reports per-phase average milliseconds, the flattened source size in bytes, and end-to-end throughput in bytes/second. Output is deterministic in structure; timings are machine-dependent.
- `tiq bench` exits 1 if a target cannot be read, and 2 for no targets or a non-positive `-i`. The numbers establish the baseline tracked in OPTIMIZATION_PLAN.md (M21).

- `tiq init [name]` scaffolds a package manifest. With no argument it writes the deterministic template to `tiq.toml` for the default package name `my-package`; with an argument it writes `<name>.tiq.toml` with `name = "<name>"` (see "Package manifests" below for the template). It refuses to clobber an existing manifest: an existing target exits 1 with `<path> already exists` on stderr and nothing is written. An invalid package name (empty, `.`/`..`, or any character other than ASCII letters/digits/`-`/`_`/`.`) exits 2 before any file is touched.
- `tiq init --check <file.tiq.toml>` validates an existing manifest without writing anything: it exits 0 when the file parses and satisfies the manifest rules, and exits 1 with one or more located `path:line: error[E30]: ...` diagnostics on stderr otherwise. An unreadable or missing file exits 1 with `cannot read` on stderr. Unknown options, `--check` without an argument, and extra positional arguments exit 2.

- `tiq cache clear` removes all cached entries from the compiler's artifact cache directory (`/tmp/.tiq-cache`). Exits 0 on success, 1 if the removal fails.
- `tiq cache <path>` prints the cache entry path for a source file when the file has a valid cache entry (exit 0), or prints `not cached: <path>` to stderr and exits 1 when the file is not cached or the cache entry is stale (source content changed). The cache uses an FNV-1a content hash of the source file for validation; a cache entry is valid only when the stored hash matches the current source content. Unknown options and extra arguments exit 2.

### Package manifests

Package manifests are INI-style `*.tiq.toml` files with three recognized sections — `[package]`, `[deps]`, and `[tests]` — whose bodies are `key = value` lines, `#` comments, or blank lines. Values may be bare or quoted (`"..."` or `'...'`, quotes stripped). `[package]` requires a non-empty `name`; a package name is a non-empty string of ASCII letters, digits, `-`, `_`, and `.`, and must not be `.` or `..`. `version` must be `major.minor.patch`, three dot-separated runs of digits with no empty part. Valid keys are `name`, `version`, `description`, `license`, `repository`, and `src` in `[package]`; `[deps]` entries are dependency names mapped to sources (`name = value`, both non-empty); `[tests]` accepts `dir` and `include`. The full rules and diagnostics are in LANGUAGE_SPEC §18.2.

The `tiq init` template is deterministic and looks like:

```text
# Tiq package manifest
[package]
name = "my-package"
version = "0.1.0"
description = "A Tiq package"

[tests]
dir = "tests"
```

An explicit name replaces `my-package` in `name` and in the file name (`<name>.tiq.toml`).

### Canonical formatting rules

The formatter re-emits the token stream with the repository's canonical layout, and is idempotent (`fmt(fmt(x)) == fmt(x)`):

- One space on either side of binary, comparison, assignment, and declaration operators: `a + b`, `x = 1`, `r <- "double"`, `b == 0`, `n % 2`, `x += 1`, `a < b`, `f a -> i64 -> ...`, `x ?? 0`, `_ => y`, `pt:Point`.
- Unary `+`, `-`, `&`, and `!` stay tight against their operand (`-x`, `-121`, `&mut x`, `!flag`, `bool!str`), and `move` keeps a trailing space.
- `?` takes a trailing space except when it opens a guard (`?[cond]`) or is the propagation operator (`a?`); a `{` following `]`, `->`, or an identifier is glued onto the line (`[0..11] {`, `?[cond] {`, `f x -> {`, `struct Point {`, `match x {`) and opens a new line with one more indent level.
- Record literals written on one source line stay inline (`Point { x: 3, y: 4 }`); a record literal that spans lines formats as a block. Struct, enum, and match bodies always format as blocks.
- Ranges are tight (`0..5`, `xs[1..3]`, `[1; 3]`); the stream operator `...` takes a trailing space (`[0, 1, ... a + b]`).
- Calls, indexes, and field access stay tight (`print(f(x))`, `fib[i]`, `vec[Token]`, `pt.x`).
- `,` and `;` take a trailing space (`print(i); break`, `[1; 3]`, `gcd(b, a % b)`).
- Comments are trivia attached to tokens and are re-emitted verbatim in place: comment-only lines stay on their own line (indented inside blocks), trailing comments stay on the same line as their statement, and a final newline is inserted when the input lacks one. Blank lines are preserved.

## Option notes

- `tiq build`: `--target <triple>` is forwarded to the host C compiler; cross-compilation targets are planned but not tested (M11).
- Unknown commands fail closed: `tiq` prints usage to stderr and exits with code 2.

## Planned

```text
tiq run <file.tiq> [-- program-args]
tiq build <package> --release
```

Developer tooling is implemented as Tiq programs (`src/tiq/tools/*.tiq`) after self-hosting (POST_BOOTSTRAP_ROADMAP M14); see "Developer tooling" above. Planned:

```text
tiq lsp [--root <path>]
```

The removed C implementations remain available in git history.

## Exit codes

```text
0   success
1   source, semantic, backend, or host compiler failure
2   invalid CLI usage
```

Diagnostics go to stderr. Generated program output goes to stdout. Commands must be deterministic for identical inputs, compiler version, target, and build options.

## Build profiles

Planned profiles:

```text
dev      fast compiler feedback and debug information
release  balanced runtime speed and size
tiny     optimize size, strip optional metadata
```

No profile may change language semantics.
