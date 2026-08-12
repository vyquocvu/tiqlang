#!/bin/sh
# Contract test for the opt-in cross-language runtime benchmark.
set -eu

OUT=$(python3 benchmarks/language_compare/run.py --check)
printf '%s\n' "$OUT" | grep -Fq \
  'benchmark check: ok (5 cases; tiq,c,go,rust,python)'

echo "language_benchmark: ok"
