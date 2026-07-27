CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq

.PHONY: all clean test test-unit test-fuzz example bench

all: $(TIQ)

SRCS = src/main.c src/emit_c.c src/lexer.c src/diag.c src/parser.c src/semantic.c \
       src/type.c src/arena.c \
       src/formatter.c src/cache.c src/tester.c src/manifest.c src/lsp.c \
       src/benchmark.c
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

# Tooling tests
test-fmt: $(TIQ)
	@# Test formatter on examples (--check unmasked since fmt preserves comments)
	$(TIQ) fmt --check examples/hello.tiq
	$(TIQ) fmt examples/fib.tiq > /dev/null
	@echo "fmt: ok"

test-check: $(TIQ)
	@# Test check command
	$(TIQ) check examples/hello.tiq
	$(TIQ) check examples/fib.tiq
	@echo "check: ok"

test-init: $(TIQ)
	@# Test init command
	rm -rf /tmp/tiq-test-init
	mkdir -p /tmp/tiq-test-init
	(cd /tmp/tiq-test-init && $(CURDIR)/$(TIQ) init mytest)
	grep -q 'name = "mytest"' /tmp/tiq-test-init/mytest.tiq.toml
	rm -rf /tmp/tiq-test-init
	@echo "init: ok"

test-cache: $(TIQ)
	@# Test cache command
	$(TIQ) cache path | grep -q tiq
	$(TIQ) cache clear
	@echo "cache: ok"

test-run: $(TIQ)
	@# Test run command
	OUTPUT=$$($(TIQ) run examples/hello.tiq)
	[ "$$OUTPUT" = "Hello from Tiq" ]
	@echo "run: ok"

test-bench: $(TIQ)
	@# Test benchmark command
	$(TIQ) bench -q examples/
	$(TIQ) bench -v -i 3 examples/hello.tiq > /dev/null
	@echo "bench: ok"

test-all: test-fmt test-check test-init test-cache test-run test-bench

test-tooling: $(TIQ)
	sh tests/tooling.sh

bench: $(TIQ)
	@echo "=== Benchmark: all examples ==="
	$(TIQ) bench examples/

bench-quick: $(TIQ)
	$(TIQ) bench -q examples/ -i 5

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf $(BUILD) ~/.cache/tiq ~/.cache/tiq-tests
