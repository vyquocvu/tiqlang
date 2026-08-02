# M13 Determinism Inventory (M13.1 closure audit)

Date: 2026-07-31 (M13.1-P7). Status: normative reference for the **M13.6 byte-identical
bootstrap gate** and for the S2–S5 Tiq ports.

Scope: static audit of the C bootstrap emission pipeline as of M13.1 closure
(Phase 0, P1–P6, P8 landed; M13.2-S1 landed). Sources audited: `src/main.c`,
`src/lexer.c`, `src/parser.c`, `src/semantic.c`, `src/type.c`, `src/emit_c.c`,
`src/module.c`, `src/diag.c`, `src/arena.c`, `include/runtime_prelude.h`,
`include/runtime_aux.h`, `include/diag.h`. Verification harnesses:
`tests/determinism.sh` (Phase 0: emit-c twice, byte-diff) and `tests/module.sh`
case 8 (multi-module byte-identity + no-filesystem-path pin).

Because another agent may be editing sources concurrently, claims below cite
**file + function**, not line numbers.

## 1. Iteration orders that affect emitted C

The invariant: **every container the compiler itself iterates is a linear array
in insertion/declaration order; the compiler uses no hash containers at all.**
Verified by grep over `src/*.c`: no `qsort`, no hashing, no `rand`; the only hash
table in the repository is the *runtime* map (`runtime_aux.h` AUX11), which
exists only inside compiled programs, never inside the compiler.

| # | Ordered structure | Where (file / function) | Why deterministic |
|---|---|---|---|
| 1 | Module load order | `src/module.c` / `load_module` | DFS from the root file, children visited in `import` declaration order; each module appended in **post-order**; first visit wins (canonical-path dedupe skips re-loads silently); depth cap `TIQ_MAX_IMPORT_DEPTH` (64) fails closed with E28-adjacent diagnostics, never reorders. |
| 2 | Module flattening | `src/emit_c.c` / `compile_modules_to_c` | Post-order module statement lists concatenated into one array; `AST_IMPORT` nodes stripped in place. All later passes iterate this single array front-to-back. |
| 3 | Top-level emission passes | `src/emit_c.c` / `compile_modules_to_c` | Fixed pass sequence over the flattened list: struct defs → enum constants → stream-gen forward decls → function forward decls → `main` body → stream-gen defs → function defs. Each pass is a linear scan in declaration order. |
| 4 | Enum registry | `src/semantic.c` / `enum_register`; `src/emit_c.c` / `emit_enum_lookup` + enum-constant pass | Linear array, insertion order = declaration order; variants stored in declaration order; emitted constant **values are the declaration index** (`tiq_enum_<Name>_<Variant> = <idx>`). `emit_enum_lookup` is a linear scan of the flattened top-level list. Zero-variant enums emit nothing. |
| 5 | Struct registry | `src/semantic.c` / `struct_register` (`StructEntry` array); `src/emit_c.c` struct pass | Linear array, insertion order; struct C typedefs emitted in declaration order with fields in declaration order. Nominal lookup by name is a linear scan. |
| 6 | Function registry | `src/semantic.c` / `func_lookup` (`FuncEntry` array) | Linear array; `func_lookup` deliberately scans **backwards** so the most recent definition wins — a fixed rule, not an ordering hazard, but the Tiq port must replicate the direction. |
| 7 | Type pool | `src/type.c` / `intern`, `type_get_struct`, `type_get_vec` | Linear first-match scan then append — no hashing. Source comment at `type_get_vec`: pool order is *load-bearing* for emission. Pool indices depend only on the order intern requests are made, which is fixed by the (deterministic) check/emit traversal order. |
| 8 | Symbol environment | `src/semantic.c` / `env_define`, `env_lookup` | Linear array, linear scan; scope handling by index snapshot, no reordering. |
| 9 | `fresh_str_fns` set | `src/emit_c.c` / `collect_fresh_str_fns` | File-static array, but reset to count 0 and refilled from the flattened top-level list in declaration order on every compile; classification of each function is order-independent (documented in the source comments). |
| 10 | Hoisted temporaries | `src/emit_c.c` / `hoist_collect` | Post-order AST walk → temporaries evaluate left-to-right, inner before outer; IDs from `tmp_counter` (see §2). Per-statement overflow (`TIQ_MAX_HOIST` 16) fails closed (leak, no reorder). |
| 11 | Stream generators | `src/emit_c.c` / stream-gen collection (`stream_gens`, cap `TIQ_MAX_STREAM_GENS` 64) | Collected in declaration order during the linear top-level scan; forward decls and definitions emitted in that same order. |
| 12 | Scope-free sequences | `src/emit_c.c` / `emit_scope_frees`, `emit_jump_frees`, `emit_exit_frees` | Owners freed in **reverse declaration order**, innermost scope first — order derived from the scope stack, itself fed by the deterministic statement walk. |

## 2. Name mangling inventory

All identifiers the emitter invents. None derive from pointers, hashes, or the
environment; the only counter is reset per compilation.

| Scheme | Producer (file / function) | Determinism note |
|---|---|---|
| `tiq_enum_<Name>_<Variant>` | `src/emit_c.c` enum-constant pass + variant-reference emission | Names from source tokens; values = declaration index. |
| `tiq_gen_<name>` | `src/emit_c.c` stream-gen emission (binding/function name) | Name from source token; collision-free by semantic uniqueness rules. |
| `tiq_tmp<N>` | `src/emit_c.c` / `hoist_collect` + `EmitContext.tmp_counter` | Counter is a member of `EmitContext`, **zero-initialized in `compile_modules_to_c`'s struct initializer** — resets per compilation; monotonic across the whole translation unit (M9.2-F). |
| `tiq_old` | `src/emit_c.c` mutable-owner reassignment (M9.2-D) | Fixed name inside a braced block; no counter. |
| `tiq_exit_code` | `src/emit_c.c` statement-level `proc_exit` (M9.2-E) | Fixed name inside a braced block. |
| `tiq_fn_ret` | `src/emit_c.c` function epilogue (`ret_fresh_str` / scalar-result paths, M9.2-C/G) | Fixed name, one per function body. |
| `tiq_alloc`, `tiq_str_dup`, `tiq_argc`, `tiq_argv` | `include/runtime_prelude.h` (`TIQ_CORE_RUNTIME_PRELUDE`) | Verbatim string constant. |
| `tiq_*` runtime helpers (e.g. `tiq_json_get`, `tiq_net_fetch`, `tiq_str_sub`, `tiq_fs_list`, `tiq_vec_*`, `tiq_sb_*`/strbuf, `tiq_map_*`) | `include/runtime_aux.h` AUX1–AUX11 | Verbatim string constants (see §4). |
| User names | pass-through | Emitted verbatim from source tokens (`%.*s` on the token span); no renaming, no gensym. |

## 3. Environment leakage audit

Finding: **nothing environment-derived reaches emitted C.** Evidence per vector:

| Vector | Finding | Evidence |
|---|---|---|
| Absolute / filesystem paths | Never emitted. `realpath` canonicalization in `src/module.c` / `load_module` is a **dedupe key and cycle-detection key only**; diagnostics print the path *as written*; the module header comment states nothing filesystem-derived enters generated C. | Pinned by `tests/module.sh` case 8: `grep -qF "$TMP_DIR"` over the emitted C must find nothing. |
| Environment variables | `getenv` appears only in `src/main.c` (`TMPDIR` for the temp `.c` file, `CC` for the host compiler) — both on the `build` path, after emission; `emit-c` streams generated C straight to stdout. | grep over `src/`: only two `getenv` sites, both in `main.c`. |
| Timestamps / randomness | None. No `time`, `clock`, `srand`, `rand` anywhere in `src/`. | grep. |
| Pointer values | No `%p` in any format string in `src/`. | grep. |
| Locale | No `setlocale`, `strcoll`, `strxfrm`, `strftime` in `src/`. All string comparison is `strcmp`/`strncmp`/`memcmp` (byte-wise, locale-independent). | grep. |
| `qsort` (stability hazard) | Not used — neither in the compiler nor in the runtime prelude. The only sort in the system is `tiq_fs_list`'s **insertion sort with `strcmp`** (`runtime_aux.h` AUX8) — byte-wise, stable, locale-free, per the LANGUAGE_SPEC §19.6 "mandatory for deterministic output" clause. | grep + AUX8 source. |
| Float formatting at compile time | The compiler never formats floating-point values into emitted C: numeric literals are emitted **verbatim from source token text** (`src/emit_c.c` literal emission, `%.*s` over the token span; integers get an `LL` suffix). The `printf("%g\n", ...)` seen in emit_c.c is a *constant string* placed into the generated program (runtime formatting of the compiled program's output), not compile-time formatting — the emitted bytes are fixed. | `src/emit_c.c` literal + `print` emission. |
| String literal escaping | `src/emit_c.c` / `emit_c_string`: validated escapes pass through verbatim; control bytes are hex-escaped with the fixed lowercase format `\x%02x`. Deterministic byte-for-byte. | `emit_c_string`. |
| Diagnostics channel | All diagnostics go to **stderr** via `src/diag.c` / `diag_error` — never interleaved into the emitted-C stdout stream. | `diag.c`. |

## 4. Runtime prelude stability (AUX1–AUX11)

- The emitted runtime is `TIQ_CORE_RUNTIME_PRELUDE` (`include/runtime_prelude.h`)
  followed by `TIQ_RUNTIME_PRELUDE_AUX1` … `AUX11` (`include/runtime_aux.h`) —
  all `static const char[]` **string constants written verbatim** by consecutive
  `fputs` calls in `src/emit_c.c` / `compile_modules_to_c`. Identical bytes on
  every run by construction.
- **4095-char constraint**: each chunk stays under the ISO C minimum string
  literal length (source comments in `runtime_aux.h` at the AUX8–AUX11
  boundaries). Growth policy is **append-only**: new runtime code goes into a
  new `AUXn+1` chunk; existing chunks are never reflowed. Reordering or
  splitting existing chunks would change every emitted program byte-for-byte
  and break the M13.6 identity gate for no functional reason — don't.
- Chunk inventory relevant to M13.1: AUX8 = P1 (`tiq_str_sub`, `tiq_str_eq`,
  `tiq_eprint`, `tiq_fs_list` with strcmp insertion sort); AUX9 = P3 (`TiqVec`,
  doubling from capacity 8, deterministic OOB aborts); AUX10 = P4 (`TiqStrBuf`,
  doubling from capacity 16, NUL-terminated); AUX11 = P5 (`TiqMap`).
- **Runtime map determinism** (AUX11): FNV-1a 64-bit with the fixed standard
  constants (offset basis `14695981039346656037ULL`, prime `1099511628211ULL`)
  — never seeded; open addressing / linear probing over power-of-two buckets
  (initial 8); load factor ≤ 0.7 via the integer check
  `(len + 1) * 10 > nbuckets * 7`; entries duplicated into **separate
  insertion-order key/value arrays**, and `map_key_at`/`map_val_at` read only
  those arrays — **iteration never touches bucket order**, so rehashing is
  invisible to programs. This is the property that lets the S3/S4 Tiq ports use
  maps for symbol tables without endangering byte-identity.
- All container failure modes abort deterministically (fixed message to stderr,
  exit 1) — never UB, never address-dependent output.

## 5. M13.6 / S2–S5 watchlist

Order-sensitive behaviors the Tiq ports must replicate **exactly**, plus known
risks for the byte-identity gate:

| # | Item | Requirement on the Tiq port |
|---|---|---|
| 1 | Type pool interning | Linear first-match scan, append on miss (`src/type.c` / `intern`). Pool *order* (hence pooled identity comparisons) must match the C bootstrap's request order. Do not "optimize" with a map. |
| 2 | Module flattening | DFS by import declaration order, first-visit dedupe by canonical path, **post-order** append, imports stripped. Canonical paths stay dedupe keys — never emitted, never in diagnostics (diagnostics use the path as written). |
| 3 | Diagnostic format | Exactly `path:line: error[E%02d]: msg\n` to stderr (`src/diag.c` / `diag_error`) — **two-digit zero-padded** code. S1's lexer already pins this differentially; S2–S4 must keep it. Fatal-code set: `ERR_UNSUPPORTED_STATEMENT` only. |
| 4 | Enum constant values | Declaration index, declaration order, zero-variant enums emit nothing (`src/emit_c.c` enum pass). |
| 5 | `tmp_counter` | Starts at 0 per compilation, monotonic across the whole translation unit — not per-function, not per-statement (`EmitContext` in `src/emit_c.c`). |
| 6 | Hidden state | `fresh_str_fns` is file-static in C but deterministically re-initialized each compile. The Tiq port must not carry any state across compilations (relevant if the driver ever compiles more than one program per process). |
| 7 | `func_lookup` direction | Backwards scan — most recent definition wins. |
| 8 | Emission pass sequence | Core prelude → AUX1–11 → structs → enums → stream-gen fwd decls → fn fwd decls → `main` → stream-gen defs → fn defs. Any pass reorder breaks byte identity. |
| 9 | Free-sequence orders | Scope frees reverse-declaration-order after defers; jump/exit frees innermost-first; hoist frees newest-first (`emit_scope_frees` / `emit_jump_frees` / `emit_exit_frees` / hoist epilogue). |
| 10 | Fixed caps and fail-safe overflows | Stream gens 64, hoists/statement 16, scope depth 64, import depth 64. Overflow behavior is part of observable determinism (leak-not-reorder / fail-closed) and must match. |
| 11 | String escaping | `emit_c_string` semantics: pass validated escapes through, hex-escape control bytes as lowercase `\x%02x`. |
| 12 | Literal pass-through | Numeric literals emitted verbatim from token text (`LL` suffix for ints). The port must carry token spans, not re-format numbers. |
| 13 | Self-applied map usage | When the Tiq compiler uses `map`/`vec` builtins for its own tables (S3+), only insertion-order iteration (`map_key_at`/`map_val_at`, len-snapshot scoping) is byte-identity-safe. Never derive emission order from lookup patterns or bucket state. |
| 14 | S1 `str_sub` workaround | `src/tiq/lexer.tiq` deliberately keeps `str_sub`-based char reads even after the P8 `s[i]` fix; switching it to `s[i]` is allowed only if the differential harness stays green — output identity, not source style, is the invariant. |

Verification anchors for M13.6: `tests/determinism.sh` (single-file double-run
byte-diff over 5 fixtures) and `tests/module.sh` case 8 (multi-module double-run
byte-diff + path-leak grep). The 3-stage convergence gate is implemented by
`tests/bootstrap.sh`: it builds `build/tiq-stage1` from the C bootstrap, runs
`tiq-stage1 emit-c src/tiq/emit_c_main.tiq` → `build/stage1.c`, rebuilds
`build/tiq-stage2` from `stage1.c` using the same host-cc flags as the C
bootstrap's `build` command, runs `tiq-stage2 emit-c src/tiq/emit_c_main.tiq` →
`build/stage2.c`, and asserts the two outputs are byte-identical. The
convergence property proves the self-hosted compiler is a fixed point under
self-application.

## Appendix A — Discrepancies (change-boundary audit, P1–P6 + P8)

Found by cross-checking LANGUAGE_SPEC.md, GRAMMAR.md, include/diag.h,
IMPLEMENTATION_STATUS.md, ROADMAP.md, POST_BOOTSTRAP_ROADMAP.md. Per P7 rules,
these are **listed, not fixed** (spec/grammar edits belong to their owning
packages).

| # | File | Discrepancy | Severity |
|---|---|---|---|
| D1 | `docs/GRAMMAR.md` (bootstrap-notes paragraph, near end) | "`struct` definitions and record literals do not parse." — stale since M12.6; contradicts the same file's `struct_def`/`record_lit` productions (marked ✅) and LANGUAGE_SPEC §17.2. Known item, deliberately left by P2. | Cosmetic (doc-only; no effect on emitted C) |
| D2 | `docs/IMPLEMENTATION_STATUS.md` ("Current milestone" header) | Says "M8 — User-defined composite types…" while dated M12/M13.1/M13.2 entries below prove work far past M8. | Cosmetic |
| D3 | `docs/ROADMAP.md` §M11 | Ticked items M11.1/M11.2 cite `src/lsp.c` and `tests/tooling/lsp.sh` as present-tense evidence; both were removed in the 2026-07-30 bootstrap scope reduction. Unlike §M5, §M11 carries no "removed from bootstrap; historical record" note. | Cosmetic |

No blockers for M13.6 were found: every determinism-relevant claim in the docs
matches the sources.

**Verified in agreement (no discrepancy):**

| Package | Spec | Grammar | Diag codes | Status entry |
|---|---|---|---|---|
| Phase 0 | n/a (test-only) | n/a | n/a | ✅ dated 2026-07-30 |
| P1 str/IO builtins | §19.5, §19.6, §16.4 owned-list | n/a (no syntax) | E09/E12 reuse | ✅ 2026-07-30 |
| P2 enums | §17.5 (+§4 keyword, tier table) | `enum_def` production | E24/E25/E26 in spec §17.5 **and** `include/diag.h` (values 24–26) | ✅ 2026-07-31 |
| P3 Vec | §19.7 | n/a (builtins only) | E09/E12 reuse | ✅ 2026-07-31 |
| P4 StrBuf | §19.8 | n/a | E09/E12 reuse | ✅ 2026-07-31 |
| P5 Map | §19.9 | n/a | E09/E12 reuse | ✅ 2026-07-31 |
| P6 modules | §17.6 (+§2, §4) | `import_decl` in `program` | E27/E28 in spec §17.6 **and** `include/diag.h` (values 27–28) | ✅ 2026-07-31 |
| P8 s[i] + annotations | §7, §13.1 bounds rule, §19.10 (+§19.7–§19.9 boundary notes) | `container_type`, `function_def`/`param_type` | E04/E09/E12/E23 reuse; E28 remains last assigned | ✅ 2026-07-31 |

`POST_BOOTSTRAP_ROADMAP.md` M13.1's four sub-items (modules, enums, vec+map,
strbuf) are all landed and evidenced above; the M13.1 checkbox is ticked as part
of this audit. `docs/ROADMAP.md` intentionally has no M13 section (self-hosting
lives in POST_BOOTSTRAP_ROADMAP.md; ROADMAP M11's single "Self-hosting Tiq
compiler" line correctly remains unticked).
