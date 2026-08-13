# Tiq Dogfooding Plan

Status: active, pre-beta design validation.

Tiq must prove that its small language surface can build real tools, not only
compiler fixtures. Dogfooding is therefore a product-design gate: the programs
below must be written in Tiq, exercised on realistic inputs, and used to collect
evidence before the v1.0 syntax and compatibility lock.

## Ground rules

- Keep the application logic in Tiq. Platform access may use documented `std/`
  modules or explicit FFI, but a C implementation hidden behind a single
  application-specific wrapper does not count.
- Check correctness before measuring speed. Every benchmark input has a pinned
  expected result or a byte-for-byte round-trip oracle.
- Record compiler version, backend, host, compiler flags, input corpus, warm-up,
  iteration count, elapsed time, throughput, peak RSS, and output size.
- Separate compile time from run time and report medians plus dispersion. Never
  present a single best run as the result.
- Keep discovered friction visible. Do not add syntax merely to shorten one
  benchmark; first show that the problem recurs and cannot be solved clearly by
  a library API.
- Any language change follows the normal vertical-slice rule: failing test first,
  specification and grammar (when syntax changes), both compilers, diagnostics,
  backends, and implementation status must agree.

## Task 3.1 — JSON parser and generator

Build a JSON implementation whose parser, in-memory representation, traversal,
mutation, and generator are authored in Tiq. The existing `std/json.tiq` wrappers
over runtime builtins are useful application primitives, but using them as the
parser under test does not satisfy this task.

### Correctness gate

- Accept the JSON grammar for objects, arrays, strings, numbers, booleans, and
  null; reject trailing data, invalid escapes, invalid UTF-8, malformed numbers,
  and truncated input deterministically.
- Report failures with byte offset and line/column at the application boundary.
- Cover empty and deeply nested documents, with an explicit configured nesting
  limit so hostile input cannot exhaust the C stack silently.
- Decode escapes and Unicode consistently, and define whether object key order
  and duplicate keys are preserved, rejected, or normalized.
- Round-trip a checked-in corpus and compare semantic results with an independent
  reference implementation. Add deterministic regression fixtures for every bug.
- Run under ASan/UBSan and demonstrate that success and rejection paths do not
  leak owned allocations.

### Performance gate

Measure parse-only, generate-only, and parse-modify-generate workloads over
small messages, a representative API corpus, a large flat document, and a deeply
nested document. Compare against simdjson and RapidJSON only with the same corpus,
validation requirements, allocation accounting, and output semantics. Publish
both throughput and latency, plus peak memory; the purpose is diagnosis rather
than claiming parity from incomparable modes.

### Design questions to record

- Is pattern matching convenient and exhaustive for JSON value dispatch?
- Can recursive data be represented without unsafe or opaque workarounds?
- Are slices and string views ergonomic while making lifetimes visible?
- Does explicit allocator propagation improve control, or dominate function
  signatures and error paths?
- Do `Option` and `Result` preserve useful payload types through parser layers?
- Can containers express object lookup and ordered iteration without excessive
  parallel arrays or copying?

## Task 3.2 — Real CLI or HTTP service

Status: routed HTTP service implemented in `src/tiq/tools/router.tiq`; behavioral
and sanitizer evidence lives in `tests/router_tool.sh`. Benchmark and friction
reports remain open, so this task has not passed the beta gate.

Build one stateful CLI (task manager or dotfile manager) or a small routed HTTP
service entirely in Tiq. The existing `tiq proxy` is the first network artifact,
but this task adds persistent state or route dispatch rather than only forwarding
bytes.

### CLI acceptance gate

- Support create/list/update/delete or plan/apply/status operations with a stable
  command-line contract and useful exit codes.
- Persist data atomically: write a temporary file, flush/close it, then replace
  the destination; interrupted writes must not corrupt the last valid state.
- Test invalid commands, malformed state, missing files, permission failures,
  idempotent operations, deterministic listing, and paths containing spaces.

### HTTP acceptance gate

- Route by method and path, parse bounded request bodies, and emit correct status,
  Content-Length, content type, and connection behavior.
- Include at least static, parameterized, not-found, method-not-allowed, malformed,
  and oversized-request paths.
- Apply explicit request, header, body, and recursion limits; failures must not
  crash the service or leave a partial response presented as success.
- Benchmark fixed route mixes at concurrency levels the implementation actually
  supports. A sequential server must be labeled sequential until the M19 event
  loop is used.

### Design questions to record

- Are filesystem, process, socket, and HTTP APIs composable without hidden state?
- Are error propagation and cleanup concise on early returns?
- Is configuration parsing deterministic and adequately diagnosed?
- Does allocator ownership remain understandable across request or command
  boundaries?
- Can route or command dispatch stay readable without new special syntax?

## Feedback record and beta gate

Each artifact must check in a short report containing:

1. the exact Tiq revision and reproducible commands;
2. correctness, sanitizer, and benchmark results;
3. friction observations grouped as library, tooling, diagnostic, performance,
   or language-design issues;
4. the smallest library-level remedy tried before proposing syntax;
5. accepted changes, rejected alternatives, and migration notes for breaking
   changes.

Breaking changes are expected before beta when evidence shows a recurring design
failure. They are not accepted from benchmark results alone, and beta does not
start while either selected artifact relies on undocumented compiler behavior,
unbounded hostile-input recursion, or an application-specific C escape hatch.
