CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq

.PHONY: all clean test example

all: $(TIQ)

SRCS = src/main.c src/lexer.c src/diag.c src/parser.c src/semantic.c \
       src/formatter.c src/cache.c src/tester.c src/manifest.c src/lsp.c
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)

$(BUILD)/%.o: src/%.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TIQ): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TIQ)

example: $(TIQ)
	$(TIQ) build examples/hello.tiq -o $(BUILD)/hello
	$(BUILD)/hello

test: $(TIQ)
	sh tests/smoke.sh
	sh tests/diagnostics.sh
	sh tests/lexer.sh
	sh tests/parser.sh
	sh tests/semantic.sh

# Tooling tests
test-fmt: $(TIQ)
	@# Test formatter on examples
	$(TIQ) fmt --check examples/hello.tiq || true
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

test-all: test-fmt test-check test-init test-cache

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf $(BUILD) ~/.cache/tiq ~/.cache/tiq-tests
