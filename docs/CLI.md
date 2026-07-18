# Tiq CLI

Canonical executable: `tiq`.

## Implemented

```text
tiq --version
tiq emit-c <file.tiq>
tiq build <file.tiq> [-o output]
```

## Planned

```text
tiq run <file.tiq> [-- program-args]
tiq check <file.tiq>
tiq fmt <paths...>
tiq test [path]
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
