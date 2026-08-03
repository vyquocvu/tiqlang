CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq

.PHONY: all clean test test-unit test-fuzz example test-check test-run tool-test tool-fmt tool-bench tool-init tool-cache tool-lsp tool-install tool-registry tool-std

# Build the unit runner with the same flags as the compiler. Besides keeping
# `make` useful as a complete build gate, this preserves sanitizer link flags
# for the documented two-step `make CFLAGS=...` then `make test` workflow.
all: $(TIQ) $(BUILD)/unit_tests

SRCS = src/main.c src/emit_c.c src/lexer.c src/diag.c src/parser.c src/semantic.c \
       src/type.c src/arena.c src/module.c
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

example: $(TIQ)
	$(TIQ) build examples/hello.tiq -o $(BUILD)/hello
	$(BUILD)/hello

test: $(TIQ) test-unit
	sh tests/smoke.sh
	sh tests/diagnostics.sh
	sh tests/lexer.sh
	sh tests/parser.sh
	sh tests/semantic.sh
	sh tests/examples.sh
	sh tests/determinism.sh
	sh tests/module.sh
	sh tests/selfhost_lexer.sh
	sh tests/selfhost_parser.sh
	sh tests/selfhost_semantic.sh
	sh tests/selfhost_emit_c.sh
	sh tests/bootstrap.sh
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

test-check: $(TIQ)
	sh tests/check.sh

test-run: $(TIQ)
	sh tests/run.sh

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

# M15: verify std/ module gating — domain builtins require import, core
# builtins remain always available, cwd fallback, wrapper correctness, ASan.
tool-std: $(TIQ)
	sh tests/std_mod.sh

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf $(BUILD)
