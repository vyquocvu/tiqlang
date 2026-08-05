# M17.3 Implementation Session — 2026-08-05

## Summary

Completed M17.3.1 (Integrated Mach-O relocatable-object writer for arm64 macOS) and M17.3.2 (Integrated Mach-O executable linker for arm64 macOS). The QBE native-code pipeline — source → IR → QBE IL → qbe assembly → in-process assembler (`.o`) → in-process linker (self-signed `MH_EXECUTE`) — now runs entirely without an external assembler or linker on the Darwin arm64 host. Both milestones are fully verified under the standard build and the ASan/UBSan build.

---

## M17.3.1 — Mach-O Object Writer (COMPLETE)

### Files
- `src/asm_arm64.c` (~1.1k lines) — single-pass assembler for the QBE arm64 Mach-O assembly subset
- `src/macho_obj.c` — deterministic minimal MH_OBJECT writer
- `include/asm_arm64.h` — public API for the assembler + object writer
- `tests/macho_obj.sh` — wired into `make test`
- `src/main.c` — `tiq emit-obj`; `build_qbe` assembles in-process instead of `cc -c`
- `Makefile`, `docs/CLI.md`, `docs/IMPLEMENTATION_STATUS.md`, `docs/POST_BOOTSTRAP_ROADMAP.md`

Details: single-pass assembly with label backpatching, ALU/mov/adrp+@PAGE/load-store/branch/extension/cset/FP encodings, data directives (`.text`/`.data`/`.bss`/`.globl`/`.balign`/`.ascii`/`.byte`/`.short`/`.int`/`.quad`/`.fill`). Anything outside the subset fails closed with a located diagnostic. The writer emits a single anonymous segment holding `__text`/`__data`/`__bss` with extern relocations and no timestamps/UUIDs, so repeated emission is byte-identical.

---

## M17.3.2 — Mach-O Executable Linker (COMPLETE)

### Files
- `src/macho_read.c` — bounds-checked MH_OBJECT reader
- `src/link_macho.c` (~1.1k lines) — deterministic, self-signed MH_EXECUTE linker
- `include/macho_link.h` — public API for the reader + linker
- `tests/object_link.sh` — wired into `make test`
- `tests/unit/test_main.c` — unit tests for `macho_read` + `link_macho_exec`
- `src/main.c` — `tiq link-qbe` command; `build_qbe` links through the integrated linker by default with a `TIQ_QBE_LINK=<cmd>` escape hatch

### Technical Implementation

**Reader (`src/macho_read.c`)** parses the MH_OBJECT subset produced by `macho_obj.c` and by clang for the QBE runtime: anonymous/standard segments with `__text`/`__cstring`/`__data`/`__bss`/`__common`/`__compact_unwind` sections; extern relocations `UNSIGNED`/`SUBTRACTOR`/`BRANCH26`/`PAGE21`/`PAGEOFF12`/`GOT_LOAD_PAGE21`/`GOT_LOAD_PAGEOFF12`/`POINTER_TO_GOT`; defined/undefined/common symbols. Symbols, sections, and relocations are bounds-checked; anything outside the subset fails closed with a diagnostic.

**Linker (`src/link_macho.c`)** combines parsed objects into a runnable arm64 `MH_EXECUTE`:
- Layout: `__PAGEZERO`, `__TEXT` (`__text` + `__stubs` + `__stub_helper` + `__cstring`), `__DATA` (`__data` + `__common` zerofill + `__la_symbol_ptr` + `__got`), `__LINKEDIT`.
- Non-PIE, absolute addresses — no rebase stream, byte-identical on repeat.
- Symbol resolution: undefined externs resolve against all objects first, then a fixed libSystem export whitelist (the exact set `runtime_qbe.c` imports plus `dyld_stub_binder`); anything unresolved fails closed naming the symbol. Common symbols (`tiq_argc_global`, `tiq_argv_global`) materialize in `__common`.
- Relocations: `UNSIGNED`/`POINTER_TO_GOT` write absolute addresses, `BRANCH26` write pc-relative offsets, `PAGE21`/`GOT_LOAD_PAGE21` write page deltas, `PAGEOFF12`/`GOT_LOAD_PAGEOFF12` write 12-bit offsets (with ldr/x scaling); GOT references get a shared per-symbol `__got` slot.
- Classic dyld bind opcodes: lazy binding of libSystem calls via `__stubs`/`__la_symbol_ptr`/`__stub_helper`, plus non-lazy binding of `__got[0]` to `dyld_stub_binder` and of the per-symbol GOT slots.
- Load commands: `LC_MAIN` (entry `_main`), `LC_LOAD_DYLIB` libSystem, `LC_LOAD_DYLINKER`, `LC_DYLD_INFO_ONLY`, `LC_SYMTAB`/`LC_DYSYMTAB` (indirect symbol table for stubs/la_ptr/got), `LC_BUILD_VERSION` (macOS 14.0), a content-derived deterministic `LC_UUID`, `LC_CODE_SIGNATURE`.
- A self-generated ad-hoc code signature (SHA-256 CodeDirectory over 16 KiB pages, requirements blob, SuperBlob) so the executable runs on Apple Silicon without host `codesign`.
- Determinism: no timestamps; UUID derived from input content; repeated links of identical inputs are byte-identical.

**Integration (`src/main.c`)** adds `tiq link-qbe <obj>... -o <exe>` (Darwin arm64 only; 2 for usage errors, 1 for link failures, output written only after a successful link). `build_qbe` reads the program object and `build/runtime_qbe.o`, and links through `link_macho_exec` by default; setting `TIQ_QBE_LINK=<cmd>` (e.g. `TIQ_QBE_LINK=cc`) falls back to linking through that external toolchain. Entry is `_main` (defined by the runtime), which calls the QBE-generated `_tiq_user_main`.

**Test harness (`tests/object_link.sh`, 33rd `make test` harness):** usage fail-closed (exit 2), missing input (exit 1, "cannot read", no partial output), non-Mach-O input fail-closed, unresolved symbol fail-closed naming the symbol, end-to-end equivalence with the C backend for QBE-supported programs (`hello`, `count`, `gcd`, `max`), exit-code propagation, byte-determinism of repeated links, `codesign --verify` acceptance of the self-generated signature, `tiq build --backend qbe` using the integrated linker, the `TIQ_QBE_LINK=cc` escape hatch, and a no-leftover-temp-files check. Skipped on non-Darwin arm64 hosts.

**Unit tests (`tests/unit/test_main.c`, 146 assertions):** adds `macho_read` roundtrip goldens over an emitted object, garbage rejection, and `link_macho_exec` producing a valid MH_EXECUTE header.

### Key Bug Fixes During Implementation

1. **`__data` written into `textbuf` (heap-buffer-overflow).** The pass-5 materialization loop copied every non-zerofill section — including `__data`/`__cstring` — into `textbuf` at segment-relative offsets, so a `__data` section placed in the `__DATA` segment wrote past the end of the page-aligned `textbuf` allocation. ASan caught the corruption (`WRITE of size 15` at `textbuf + text_filesize`). Fixed by making the pass-5 loop copy only text-side sections (`cls <= 1`) into `textbuf`; `__data` is copied solely into `databuf` by the dedicated data loop. (Non-sanitizer runs silently tolerated this; it is a real latent memory-corruption bug.)
2. **`src: unbound variable` in `tests/object_link.sh`.** The `local src="$1" exe="$2" base="...$(basename "$src" .tiq)"` statement expanded `$(basename "$src" ...)` in a subshell where `src` was not yet bound under `set -u`, dropping the basename and emitting a shell error per call. Fixed by splitting into separate `local` declarations.
3. **Cosmetic escape-hatch warning.** `TIQ_QBE_LINK=cc` links the M17.3.1 object (which, like clang's, carries no `LC_BUILD_VERSION`) with host `ld`, which prints `ld: warning: no platform load command found…`. Non-fatal and confined to the explicitly-host-toolchain escape-hatch path; the executable is still produced and runs correctly.

### Verification

```bash
# Standard build
make clean && make && make test
# Result: all 33 harnesses pass, including macho_obj.sh and object_link.sh; unit tests green (146 assertions).

# Sanitizer build
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined'
ASAN_OPTIONS=detect_leaks=0 make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined' test
# Result: all harnesses pass; no ASan/UBSan errors (the textbuf overflow fix is confirmed).
```

Manual smoke: `tiq build examples/hello.tiq --backend qbe` and `tiq link-qbe <prog>.o build/runtime_qbe.o -o exe` produce native executables whose output matches the C backend; `codesign --verify` accepts the self-generated signature on Apple Silicon.

---

## Session Timeline

1. Analyse the existing working tree — the M17.3.1/17.3.2 sources, integration, and tests already present.
2. Verify the integrated linker end-to-end against the C backend (`hello`, `count`, `gcd`, `max`).
3. Fix the `tests/object_link.sh` `src: unbound variable` bug.
4. Update documentation: `docs/CLI.md` (`tiq link-qbe`, integrated-linker build note, `TIQ_QBE_LINK`), `docs/IMPLEMENTATION_STATUS.md` (M17.3.2 entry), `docs/POST_BOOTSTRAP_ROADMAP.md` (M17.3.2 marked complete).
5. Run the standard `make test` — green.
6. Run the ASan/UBSan build and tests — caught a latent heap-buffer-overflow in `link_macho_exec` (data sections written into `textbuf`); fixed at `src/link_macho.c` pass 5. Re-verified both builds green.

---

## Next Steps

- M17.3.3: ELF object writer & linker (Linux `x86_64`/`aarch64`); M17.3.4: PE writer & linker (Windows `x86_64`), following the same staged test-first flow.
- M17.4/M17.5: target matrix and optional LLVM backend.
- Optionally eliminate the `TIQ_QBE_LINK=cc` escape-hatch `ld` platform warning (e.g. emit `LC_BUILD_VERSION` in the object writer).

---

## References

- Mach-O file format: `/usr/include/mach-o/loader.h`
- ARM64 relocation types: `/usr/include/mach-o/reloc.h`
- dyld bind opcodes: `/usr/include/mach-o/dyld_priv.h`
- QBE assembly subset: `third_party/qbe/arm64/emit.c`, `gas.c`

---

**Session Status:** M17.3.1 COMPLETE, M17.3.2 COMPLETE — both verified under the standard and ASan/UBSan builds, with documentation updated.
