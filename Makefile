CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

BUILD := build
TIQ := $(BUILD)/tiq

.PHONY: all clean test example

all: $(TIQ)

$(TIQ): src/main.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) src/main.c -o $(TIQ)

example: $(TIQ)
	$(TIQ) build examples/hello.tiq -o $(BUILD)/hello
	$(BUILD)/hello

test: $(TIQ)
	sh tests/smoke.sh

clean:
	rm -rf $(BUILD)
