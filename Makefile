CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq
TEST_JOBS ?= 5

.PHONY: all clean test test-heavy test-unit test-fuzz test-selfhost test-selfhost-lexer test-selfhost-parser test-selfhost-semantic test-selfhost-emit-c test-bootstrap example benchmark-compare test-check test-run test-qbe test-wasm tool-test tool-fmt tool-bench tool-init tool-cache tool-lsp tool-install tool-registry tool-publish tool-audit tool-proxy tool-router tool-json tool-std perf-record perf-check

# Build the unit runner with the same flags as the compiler. Besides keeping
# `make` useful as a complete build gate, this preserves sanitizer link flags
# for the documented two-step `make CFLAGS=...` then `make test` workflow.
all: $(TIQ) $(BUILD)/unit_tests $(BUILD)/qbe $(BUILD)/runtime_qbe.o

SRCS = src/main.c src/emit_c.c src/emit_qbe.c src/emit_wasm.c src/lexer.c src/diag.c src/parser.c src/semantic.c \
       src/type.c src/arena.c src/module.c src/ir.c src/ir_lower.c src/ir_dump.c \
       src/asm_arm64.c src/asm_amd64.c src/asm_rv64.c src/macho_obj.c src/macho_read.c src/link_macho.c \
       src/elf_obj.c src/elf_read.c src/elf_link.c \
       src/pe_obj.c src/pe_read.c src/pe_link.c
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)

# Rebuild everything when any public header changes (ABI safety).
HDRS = $(wildcard include/*.h)

$(BUILD)/%.o: src/%.c $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TIQ): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TIQ)

# Unit test harness (tests/unit): links against all objects except main.o
UNIT_OBJS = $(filter-out $(BUILD)/main.o,$(OBJS))
$(BUILD)/unit_tests: tests/unit/test_main.c $(UNIT_OBJS)
	$(CC) $(CFLAGS) tests/unit/test_main.c $(UNIT_OBJS) -o $@

test-unit: $(BUILD)/unit_tests
	$(BUILD)/unit_tests

# Deterministic seed-driven fuzz of lexer/parser via `tiq check`
test-fuzz: $(TIQ)
	sh tests/fuzz.sh

# M21.1: record the performance baseline (tests/perf_baseline.txt) on the
# current machine. Commit the result when compiler output sizes change.
perf-record: $(TIQ)
	sh tests/perf_suite.sh record

# M21.1: gate the current build against the checked-in performance baseline
# (hard gate: binary sizes; soft reports: timing and peak RSS).
perf-check: $(TIQ)
	sh tests/perf_suite.sh check

benchmark-compare: $(TIQ)
	python3 benchmarks/language_compare/run.py

example: $(TIQ)
	$(TIQ) build examples/hello.tiq -o $(BUILD)/hello
	$(BUILD)/hello

test: $(TIQ) $(BUILD)/qbe $(BUILD)/runtime_qbe.o test-unit test-heavy
	sh tests/smoke.sh
	sh tests/diagnostics.sh
	sh tests/lexer.sh
	sh tests/parser.sh
	sh tests/semantic.sh
	sh tests/examples.sh
	sh tests/determinism.sh
	sh tests/module.sh
	sh tests/makefile_parallel.sh
	sh tests/language_benchmark.sh

# These suites dominate sanitizer test time and use disjoint output paths.
# Bound the default fan-out; smaller hosts can use `make TEST_JOBS=2 test`.
test-heavy: $(TIQ)
	+$(MAKE) -j$(TEST_JOBS) test-selfhost-lexer test-selfhost-parser test-selfhost-semantic test-selfhost-emit-c test-bootstrap

test-selfhost-lexer: $(TIQ)
	sh tests/selfhost_lexer.sh

test-selfhost-parser: $(TIQ)
	sh tests/selfhost_parser.sh

test-selfhost-semantic: $(TIQ)
	sh tests/selfhost_semantic.sh

test-selfhost-emit-c: $(TIQ)
	sh tests/selfhost_emit_c.sh

test-bootstrap: $(TIQ)
	sh tests/bootstrap.sh

# Pre-M13 S5: tiered targets. test-fast covers lexer/parser/semantic/diagnostic
# tests (no sockets, network, or platform linkers) — required merge gate for
# parser/arena/semantic/emitter changes. test-selfhost covers the differential
# corpus that compares the C bootstrap and self-hosted Tiq compiler.
test-fast: $(TIQ)
	sh tests/smoke.sh
	sh tests/diagnostics.sh
	sh tests/lexer.sh
	sh tests/parser.sh
	sh tests/semantic.sh
	sh tests/examples.sh
	sh tests/determinism.sh
	sh tests/module.sh
	sh tests/surface_audit.sh

test-selfhost: $(TIQ)
	+$(MAKE) -j$(TEST_JOBS) test-selfhost-lexer test-selfhost-parser test-selfhost-semantic test-selfhost-emit-c

# Pre-M13 S5: test-backend covers backend-specific tests (current C/QBE/wasm
# emitters). test-platform covers platform-specific tests (wasm backend,
# ELF/Mach-O/PE linkers, integrated assemblers). Both are part of make
# test but callable independently.
test-backend: $(TIQ) $(BUILD)/qbe $(BUILD)/runtime_qbe.o
	sh tests/ir.sh
	sh tests/qbe_backend.sh
	sh tests/ffi.sh

test-platform: $(TIQ)
	sh tests/wasm.sh
	sh tests/macho_obj.sh
	sh tests/object_link.sh
	sh tests/elf_obj.sh
	sh tests/elf_link.sh
	sh tests/amd64_asm.sh
	sh tests/rv64_asm.sh
	sh tests/rv64_link.sh
	sh tests/pe_obj.sh
	sh tests/pe_link.sh
	sh tests/test_runner.sh
	sh tests/formatter_tool.sh
	sh tests/bench_tool.sh
	sh tests/init_tool.sh
	sh tests/install_tool.sh
	sh tests/cache_tool.sh
	sh tests/lsp_tool.sh
	sh tests/std_mod.sh
	sh tests/check.sh
	sh tests/run.sh
	sh tests/ffi.sh
	sh tests/registry_tool.sh
	sh tests/publish_tool.sh
	sh tests/audit_tool.sh
	sh tests/perf_suite_test.sh
	sh tests/proxy_tool.sh
	sh tests/router_tool.sh
	sh tests/json_tool.sh
	sh tests/ir.sh
	sh tests/qbe_backend.sh
	sh tests/wasm.sh
	sh tests/macho_obj.sh
	sh tests/object_link.sh
	sh tests/elf_obj.sh
	sh tests/elf_link.sh
	sh tests/amd64_asm.sh
	sh tests/rv64_asm.sh
	sh tests/rv64_link.sh
	sh tests/pe_obj.sh
	sh tests/pe_link.sh

test-check: $(TIQ)
	sh tests/check.sh

test-run: $(TIQ)
	sh tests/run.sh

# Issue #16: Tiq-authored JSON parser/generator dogfood artifact.
tool-json: $(TIQ)
	sh tests/json_tool.sh

# Pre-M13 S6: surface audit. Detects contradictions between the spec,
# grammar, roadmap, and implementation status (status drift). Runs as
# part of `make test` to fail closed on tier mismatches.
test-audit: $(TIQ)
	sh tests/surface_audit.sh

# M14.1: build the Tiq developer test runner via the bootstrap and exercise it
# against tests/tiq/ (pass/fail/list/verbose, skip, compile errors, fail-closed).
tool-test: $(TIQ)
	sh tests/test_runner.sh

# M14.2: build the Tiq formatter via the bootstrap and verify the canonical
# formatting rules, stdin/file equivalence, --check/--output, idempotence, and
# `--check` clean on every example.
tool-fmt: $(TIQ)
	sh tests/formatter_tool.sh

# M14.3: build the Tiq phase benchmark via the bootstrap and verify the stable
# output shape, -i parsing, directory scanning, and fail-closed exit codes.
tool-bench: $(TIQ)
	sh tests/bench_tool.sh

# M14.4: build the Tiq manifest scaffolder/validator via the bootstrap and
# verify the deterministic template, fail-closed name/path handling, --check
# validation diagnostics, usage errors, and ASan/UBSan on the emitted C.
tool-init: $(TIQ)
	sh tests/init_tool.sh

# M14.5: build the Tiq incremental module cache via the bootstrap and verify
# fail-closed usage, cache status, cache clear, and ASan/UBSan.
tool-cache: $(TIQ)
	sh tests/cache_tool.sh

# M18.1: build the Tiq package dependency installer via the bootstrap and
# verify path dep resolution, lockfile generation, fail-closed, multi-dep,
# idempotent re-run, and ASan/UBSan on the emitted C.
tool-install: $(TIQ)
	sh tests/install_tool.sh

# M14.6: build the Tiq LSP server via the bootstrap and verify the JSON-RPC
# handshake, document sync, hover, definition, semantic tokens, and
# fail-closed behavior.
tool-lsp: $(TIQ)
	sh tests/lsp_tool.sh

# M18.4: build the Tiq registry server and search tool, verify API endpoints,
# search filtering, manifest validation for registry: scheme.
tool-registry: $(TIQ)
	sh tests/registry_tool.sh

# M18.5: build the Tiq publisher and yanker, verify publish/yank workflows,
# error cases (duplicate, missing manifest/version, bad args), and search integration.
tool-publish: $(TIQ)
	sh tests/publish_tool.sh

# M18.6: build the Tiq dependency audit tool and verify integrity checks,
# tamper detection, missing deps, and error cases.
tool-audit: $(TIQ)
	sh tests/audit_tool.sh

# M21.3: build the Tiq loopback HTTP reverse proxy and verify GET/POST
# passthrough, 502 fail-closed, usage errors, and ASan/UBSan.
tool-proxy: $(TIQ)
	sh tests/proxy_tool.sh

# M21.3: build the routed Tiq HTTP service and verify route dispatch,
# bounded request parsing, fail-closed status codes, and ASan/UBSan.
tool-router: $(TIQ)
	sh tests/router_tool.sh

# M15: verify std/ module gating — domain builtins require import, core
# builtins remain always available, cwd fallback, wrapper correctness, ASan.
tool-std: $(TIQ)
	sh tests/std_mod.sh

clean:
	rm -rf $(BUILD)
	rm -f third_party/qbe/qbe

# M17.2: build QBE from vendored source
$(BUILD)/qbe: third_party/qbe/main.c
	mkdir -p $(BUILD)
	$(MAKE) -C third_party/qbe
	cp third_party/qbe/qbe $(BUILD)/qbe

# M17.2: compile the QBE-callable runtime library
$(BUILD)/runtime_qbe.o: src/runtime_qbe.c
	mkdir -p $(BUILD)
	# -O1 -fno-merge-constants keeps GCC from splitting content into
	# .text.startup/.rodata.str1.1 etc., which the integrated ELF linker
	# only merges by exact section name (.text/.data/.bss/.rodata).
	$(CC) -std=c11 -O1 -fno-merge-constants -c $< -o $@

distclean: clean
