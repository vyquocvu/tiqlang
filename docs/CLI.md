# Tiq CLI

Canonical executable: `tiq`.

## Implemented commands

The following commands match the `usage:` output of `tiq` exactly:

```text
tiq --version
tiq run <file.tiq>
tiq build <file.tiq> [-o output]
tiq emit-c <file.tiq>
tiq check <file.tiq>...
tiq fmt [--check] [--output <file>] [--use-tabs] [--indent-width <n>] [file]
tiq test [--verbose] [--list] [dir|file...]
tiq bench [-v] [-i N] [-q] <file|dir>...
tiq init [name]
tiq lsp [--root <path>]
tiq cache [clear|path]
```

### Debug / inspect commands

These commands emit intermediate representations for debugging and are not part of the primary workflow:

```text
tiq dump-tokens <file.tiq>
tiq dump-ast <file.tiq>
tiq dump-typed-ast <file.tiq>
```

## Option notes

- `tiq fmt`: `[file]` is a single optional input file; omitting it reads from stdin and writes to stdout.
- `tiq test`: `[dir|file...]` accepts zero or more directory or `.tiq` file paths; no arguments defaults to the current directory.
- `tiq bench`: `-v` is short for `--verbose`; `-q` is short for `--quiet`; `-i N` sets the iteration count.
- `tiq cache clear` removes all cached build artifacts; `tiq cache path` prints the cache directory.
- `tiq build`: accepts an undocumented `--target <triple>` flag that is forwarded to the host C compiler; cross-compilation targets are planned but not tested (M11).

## LSP server (`tiq lsp`)

`tiq lsp` speaks JSON-RPC over stdio with `Content-Length` framing. Supported methods: `initialize`, `initialized`, `shutdown`, `exit`, `textDocument/didOpen`, `textDocument/hover`, `textDocument/definition`, and `textDocument/semanticTokens/full`. All other methods and malformed requests fail closed (notifications are ignored; requests answer `null`).

On `didOpen` the server runs the full front end (lexer, parser, semantic checker) over the stored text and publishes structured `textDocument/publishDiagnostics` (M11.1): each diagnostic carries a 0-based start-of-line range, `severity` 1 (Error), `code` `"ENN"` matching the CLI error code, `source` `"tiq"`, and the exact CLI message, keyed to the stored document version. A clean document publishes the empty set. At most 16 diagnostics are published per document; further records are dropped.

## Planned

```text
tiq run <file.tiq> [-- program-args]
tiq build <package> --release
```

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

