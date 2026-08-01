CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq

.PHONY: all clean test test-unit test-fuzz example test-check test-run

all: $(TIQ)

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
	sh tests/check.sh
	sh tests/run.sh

test-check: $(TIQ)
	sh tests/check.sh

test-run: $(TIQ)
	sh tests/run.sh

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf $(BUILD)
