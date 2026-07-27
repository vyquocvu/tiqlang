# Tiq CLI

Canonical executable: `tiq`.

## Implemented

```text
tiq --version
tiq run <file.tiq>
tiq build <file.tiq> [-o output]
tiq check <file.tiq>
tiq emit-c <file.tiq>
tiq fmt [paths...] [--check] [--output file] [--use-tabs] [--indent-width n]
tiq test [-v] [-l] [path]
tiq bench [-v] [-q] [-i n] <paths...>
tiq init [name]
tiq cache (path | clear)
tiq lsp [--root dir]
tiq dump-tokens <file.tiq>
tiq dump-ast <file.tiq>
tiq dump-typed-ast <file.tiq>
```

## Planned

```text
tiq run <file.tiq> [-- program-args]
tiq build <package> --release
tiq build <package> --target <triple>
```

## Exit codes

```text
0 success
1 source, semantic, backend, or host compiler failure
2 invalid CLI usage
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
