#!/bin/sh
# Regression gate: the expensive, independent self-host/bootstrap suites must
# be dispatched through a bounded, user-configurable recursive make.
set -eu

MAKEFILE=${1:-Makefile}

grep -Fqx 'TEST_JOBS ?= 5' "$MAKEFILE"
grep -Fqx 'test-heavy: $(TIQ)' "$MAKEFILE"
grep -Fq '$(MAKE) -j$(TEST_JOBS) test-selfhost-lexer test-selfhost-parser test-selfhost-semantic test-selfhost-emit-c test-bootstrap' "$MAKEFILE"
grep -Fq 'test: $(TIQ) $(BUILD)/qbe $(BUILD)/runtime_qbe.o test-unit test-heavy' "$MAKEFILE"

echo "makefile_parallel: ok"
