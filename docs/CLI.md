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

Developer tooling is written in Tiq and built from `src/tiq/tools/` by the bootstrap (`make tool-test`, `tool-fmt`, `tool-bench` build them into `build/`). The `tiq <tool>` subcommand names below are the interface each tool implements; the C bootstrap binary dispatches them by building and running the corresponding Tiq program.

```text
tiq test [--verbose] [--list] [--tiq <compiler>] [dir|file...]
```

- `tiq test` discovers `.tiq` files (non-recursively, hidden names skipped), extracts the expected stdout from `//! expected:` marker lines (LANGUAGE_SPEC §2.1), builds and runs each test with the given compiler, and reports a `Tests: N passed, M failed, K skipped` summary on stdout. Failures and surfaced compiler diagnostics go to stderr. Exit code is 1 iff any test failed.
- `--tiq <compiler>` selects the compiler binary used to build tests (defaults to `tiq` on `PATH`).
- `--list` prints each discovered test path without running anything.
- Files without a `//! expected:` marker are skipped. Transient build artifacts are created next to each test file and removed afterwards, so paths must not contain spaces or single quotes.

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
tiq fmt [--check] [--output <file>] [--use-tabs] [--indent-width <n>] [file]
tiq bench [-v] [-i N] [-q] <file|dir>...
tiq init [name]
tiq lsp [--root <path>]
tiq cache [clear|path]
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
