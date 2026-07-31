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

## Option notes

- `tiq build`: `--target <triple>` is forwarded to the host C compiler; cross-compilation targets are planned but not tested (M11).
- Unknown commands fail closed: `tiq` prints usage to stderr and exits with code 2.

## Planned

```text
tiq run <file.tiq> [-- program-args]
tiq build <package> --release
```

Developer tooling was removed from the C11 bootstrap compiler on 2026-07-30 and will be rewritten in Tiq after self-hosting (POST_BOOTSTRAP_ROADMAP M14):

```text
tiq fmt [--check] [--output <file>] [--use-tabs] [--indent-width <n>] [file]
tiq test [--verbose] [--list] [dir|file...]
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
