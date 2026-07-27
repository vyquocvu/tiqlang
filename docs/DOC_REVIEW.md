# Documentation & Language Design Review

Date: 2026-07-27
Scope: `docs/` only — LANGUAGE_SPEC, GRAMMAR, TYPE_SYSTEM, MEMORY_MODEL, DESIGN_PRINCIPLES, COMPILER_ARCHITECTURE, CLI, ROADMAP, IMPLEMENTATION_STATUS (plus README for cross-reference).
Nature: review comments only; no code changes proposed in this document.

## Overall assessment

The design docs are unusually strong for a bootstrap-stage language: priorities are explicit (correctness > compile speed > small binaries > learnability > terseness > peak runtime), the "semantic density, not code golf" rule is well-articulated, the fail-closed principle is stated crisply, and the roadmap contains honest status audits (the 2026-07-25 corrections to M7–M11 are a good example of the project catching its own over-claims).

The main weakness is **doc drift**: several documents contradict each other or lag the implementation. This matters more here than in most projects, because `AGENTS.md` elevates doc/code agreement to a hard rule — "A language feature is incomplete unless lexer, parser, semantic checks, backend behavior, diagnostics, tests, specification, and implementation status agree." Every drift below is therefore not just a docs issue but a process violation by the project's own definition.

---

## Part 1 — Doc inconsistencies (with evidence)

### 1. The print statement `!expr` is undocumented everywhere

- **What exists:** `!expr` is the most visible syntax in the project — README line 15 (`!fib[10]`), every example in `examples/`, and M6 in IMPLEMENTATION_STATUS ("Print expression statement (`!expr`) backend emission with typed printf support").
- **The gap:** LANGUAGE_SPEC and GRAMMAR never mention a print statement. LANGUAGE_SPEC §5 documents `!` only as logical negation.
- **Why it's worse than a missing paragraph:** the token is semantically overloaded. In statement position `!x` prints; in expression position `!x` negates. And the semantic checker assigns `!x` the *operand's* type (so `!5 : int`), not `bool` — contradicting DESIGN_PRINCIPLES ("familiar operators keep conventional meanings") and LANGUAGE_SPEC §5 (`&& || !` listed as conventional). The emitter then special-cases statement-position `!` to print. So the same syntax has two meanings and a non-standard type rule, none of it written down.

### 2. README advertises a feature that does not exist

- **What README says:** line 41 — `^value   early return`.
- **Reality:** `^` is bit-xor in GRAMMAR (`bit_xor = bit_and, { "^", bit_and }`) and in LANGUAGE_SPEC §5 (`& | ^ << >>` listed as bitwise operators). No early-return syntax exists in the spec, grammar, or parser.
- **Action needed:** either spec + implement early return, or remove the line from README. Advertising unimplemented syntax in the project's front door is the worst kind of drift.

### 3. Reserved-word lists contradict each other (three lists, none agree)

- **LANGUAGE_SPEC §4:** `true false while for in break continue skip move defer`
- **LANGUAGE_SPEC §10:** bracket loops "eliminat[e] `for` and `while` keywords"
- **Lexer reality:** `break chan defer false match move mut skip spawn struct true until while`
- So: §4 reserves `for`/`in` (which have no syntax anywhere) and `while` (which §10 claims is eliminated, yet `while` survives as a clause keyword in stream bounds and predicate slicing); §4 omits six words the lexer actually reserves (`until chan spawn match struct mut`).

### 4. MEMORY_MODEL contradicts LANGUAGE_SPEC on move semantics

- **MEMORY_MODEL:** "Assignment moves owned values unless the type is explicitly copyable" — i.e. *implicit* move on assignment.
- **LANGUAGE_SPEC §16.1:** move is *explicit* via the `move x` keyword; after `y <- move x`, use of `x` is a compile-time error.
- The implementation follows §16.1 (explicit keyword, tested in `semantic.sh` / `smoke.sh`). The two normative docs describe two different ownership languages. One must yield.

### 5. GRAMMAR includes `continue`; nothing else does

- **GRAMMAR:** `control_stmt = ("break" | "continue" | "skip"), [ "if", expression ]`
- **LANGUAGE_SPEC §10:** loop control is `break` and `skip` only ("`skip`: continue iteration shorthand").
- **Lexer:** no `TOK_CONTINUE` exists.
- GRAMMAR also specifies inline guards (`break if condition`, `skip if condition`) — LANGUAGE_SPEC §10 mentions them, but the implementation status does not clearly evidence them.

### 6. Parsed-but-unspecified syntax (AGENTS.md violation)

The parser accepts syntax that appears in neither LANGUAGE_SPEC nor GRAMMAR:

- `match expr { pattern => body }` (M8)
- `struct` definitions and record literals (M8; field types stored as raw tokens)
- `chan` / `spawn` (M7; backend emits placeholder comments)
- `&x` / `&mut x` borrow syntax (M9)
- `[val; len]` array fill (M7)
- string character indexing `s[i]` (M7)
- block-body functions (M7: "reusable functions with block bodies") — GRAMMAR's `function_def` only shows an expression body

Per AGENTS.md ("Do not add syntax without updating the normative spec and grammar"), each of these is an incomplete feature by definition.

### 7. TYPE_SYSTEM's `str` model contradicts the backend

- **TYPE_SYSTEM:** "`str` is an immutable UTF-8 view represented by pointer and byte length. It is not NUL-terminated by language contract."
- **Backend reality:** emits `const char *` and `strlen()` — a NUL-terminated assumption at every call site, including `len()`.
- Only slices (`TiqSlice` with `.len`) honor the documented model. The middle state guarantees recurring contradictions.

### 8. Small spec bugs

- **Missing section:** LANGUAGE_SPEC jumps §11 → §13 (no §12).
- **Invalid example:** §9 shows `clamp x lo hi = { ... }` — with `=`, not `->`. Under the implemented grammar this does not parse as a function (the parser's `declaration()` sees `clamp` followed by identifier `x` and demands `->`). The example should read `clamp x lo hi -> { ... }`.
- **Undocumented magic names:** §14 stream generators bind context names in the generator expression, but only `a`/`b` (two-seed window) are documented. The implementation also binds `x` (single-seed previous value), `i` (index), and `s` (state) — none documented.

### 9. Stale status headers

- **TYPE_SYSTEM:** "Status: design target; only string print programs are implemented by the bootstrap slice." — far behind reality (arrays, slices, streams, move, defer, match…).
- **CLI.md:** "Implemented" lists 3 commands (`--version`, `emit-c`, `build`); the binary supports 13 (`run check fmt test bench init lsp cache dump-tokens dump-ast dump-typed-ast` plus those 3).
- **MEMORY_MODEL:** "The current compiler slice emits C string literals only and performs no heap allocation." — arrays, slices, and stream generators exist now.

---

## Part 2 — Language design updates worth making

Ordered by importance. A, B, E are pure doc edits with zero code risk. C and D shape future milestones. F is a strategic decision.

### A. Specify the print statement properly (fixes comment 1)

Three options:

1. **Minimal:** document `!expr` as a statement-only form in LANGUAGE_SPEC + GRAMMAR, and state that `!` is negation only in expression position. Closes the gap but keeps the overload wart.
2. **Cleaner:** make `!` statement-only (print) and pick a different negation (`not` keyword). Rejected — it breaks conventional `!` negation, violating the project's own top principle.
3. **Pragmatic (recommended):** keep the overload, but fix the type rule — `!expr` in expression position must yield `bool`. The print behavior belongs in the emitter as a statement-level desugar, not in the expression's type. Then document both roles explicitly.

### B. Resolve move semantics in one direction (fixes comment 4)

Keep the explicit `move` keyword — it is implemented, tested, and matches the "explicit mutation" principle (mutation and ownership transfer should be visible at the site). Rewrite MEMORY_MODEL's "Assignment moves owned values unless the type is explicitly copyable" to "Assignment copies; `move` transfers ownership and invalidates the source."

### C. Sketch error handling now (§15 is empty)

The biggest remaining design hole, and M8 depends on it. The reserved pieces (`T?`, `T!E`, `??`, `=>`) already hint at the direction:

```tiq
r = fs_read("f") ?? ""    // fallback on error
x = parse(s)?             // propagate
```

Decisions to make *before* M12.6 constructs these types:

- Are `?` / `!E` postfix type constructors or operators?
- Does `??` short-circuit, and what is its precedence relative to `?:` (which already owns `?`)?
- Is `expr?` propagation statement-level, expression-level, or both?
- How does a `T!E` value destructure — `match`, or dedicated syntax?

Writing this down now prevents M8 from being designed by the parser implementation (which is exactly how the M7–M11 status contradiction happened).

### D. Spec what already parses — or strip it (fixes comment 6)

Per AGENTS.md, two honest choices per feature:

1. Add `match` / `struct` / array-fill / borrow / `chan` / `spawn` syntax to LANGUAGE_SPEC and GRAMMAR, explicitly marked "provisional, semantics partial"; or
2. Remove them from the parser until specified.

Option 1 for `match`, `struct`, array fill, string indexing (they have real semantics). Option 2 is arguably right for `chan`/`spawn` — their backend emits `/* placeholder */ 0`, which DESIGN_PRINCIPLES explicitly forbids ("never silently... generate placeholder code"). Failing closed on unimplemented concurrency is more consistent with the language's own rules than emitting zero.

### E. Small normative fixes (fixes comments 3, 5, 8)

- Fill or renumber the missing §12.
- One reserved-word list, in LANGUAGE_SPEC §4, matching the lexer exactly: `true false break skip move defer until chan spawn match struct mut` (drop `for in while`; `while`/`until` documented as clause keywords for stream bounds, not statement keywords).
- Fix the §9 `clamp` example to use `->`.
- Remove `continue` from GRAMMAR's `control_stmt` (or implement it — but `skip` already covers it; removal keeps the syntax budget small).
- Document stream-generator context names in §14: two-seed → `a`, `b`; one-seed → `x`; always → `i` (index), `s` (state). Also state what `s` actually is in the emitted code, or remove it from the environment if unused.
- Decide `^` early-return (comment 2): recommend removing it from README — `defer` already covers the common early-exit cleanup case, and the syntax budget argues against a second control-flow operator.

### F. Decide the `str` endgame (fixes comment 7)

Two coherent end states:

1. **Bless NUL-terminated `str`** (matching the backend): simpler C interop, cheaper literals, but slicing needs care and embedded-NUL data is unrepresentable.
2. **Commit to pointer+length `str`** (matching the spec): safer slicing, matches `TiqSlice`, but the backend must migrate off `const char *`/`strlen()` and handle NUL-termination only at C boundaries.

The current middle state — spec says one thing, backend does another — is the worst option. Given that slices already exist as `TiqSlice`, option 2 is the smaller conceptual leap: `str` becomes a slice over immutable bytes, `len()` reads `.len` instead of calling `strlen()`, and NUL-termination becomes an FFI concern only.

---

## Priority summary

| Priority | Items | Type | Risk |
|---|---|---|---|
| 1 | A (print statement), B (move wording), E (normative fixes) | doc edits only | zero code risk |
| 2 | D (spec-or-strip parsed syntax) | doc + possibly parser gating | low |
| 3 | C (error-handling design) | new spec section §15 | none now; shapes M8/M12.6 |
| 4 | F (`str` endgame) | spec decision, then backend migration | medium; do with M12 work |

Items in priority 1 require no test changes and can land immediately. Item C should land before M12.6 so that Option/Result are designed on paper before they are constructed in code.
