#!/bin/sh
# Verify Tiq Quick Reference exists and covers key syntax areas

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
QR="$ROOT/docs/QUICK_REFERENCE.md"

if [ ! -f "$QR" ]; then
  echo "tiq-quick-ref: FAIL (docs/QUICK_REFERENCE.md not found)"
  exit 1
fi

# Check for key sections
required_sections="Bindings Types Functions Operators Control Flow Loops Recurrences Structs Enums Pattern Matching Option and Result"
missing=""

for section in $required_sections; do
  if ! grep -qi "$section" "$QR"; then
    missing="$missing $section"
  fi
done

if [ -n "$missing" ]; then
  echo "tiq-quick-ref: FAIL (missing sections:$missing)"
  exit 1
fi

# Check for code examples
code_blocks=$(grep -c '```tiq' "$QR" || true)
if [ "$code_blocks" -lt 10 ]; then
  echo "tiq-quick-ref: FAIL (only $code_blocks code examples, expected at least 10)"
  exit 1
fi

# Check that README links to it
if ! grep -q "QUICK_REFERENCE.md" "$ROOT/README.md"; then
  echo "tiq-quick-ref: FAIL (README.md does not link to QUICK_REFERENCE.md)"
  exit 1
fi

echo "tiq-quick-ref: ok ($code_blocks code examples verified from docs/QUICK_REFERENCE.md)"
