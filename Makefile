CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq

.PHONY: all clean test example

all: $(TIQ)

SRCS = src/main.c src/lexer.c src/diag.c src/parser.c src/semantic.c
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

clean:
	rm -rf $(BUILD)
