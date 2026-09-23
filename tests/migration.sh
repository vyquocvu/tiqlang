#!/bin/sh
# Verify Migration Guide exists and covers key languages

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MIGRATION="$ROOT/docs/MIGRATION.md"

if [ ! -f "$MIGRATION" ]; then
  echo "migration: FAIL (docs/MIGRATION.md not found)"
  exit 1
fi

# Check for key language sections
required_languages="C Rust Python Go"
missing=""

for lang in $required_languages; do
  if ! grep -qi "From $lang" "$MIGRATION"; then
    missing="$missing $lang"
  fi
done

if [ -n "$missing" ]; then
  echo "migration: FAIL (missing language sections:$missing)"
  exit 1
fi

# Check for code examples
code_blocks=$(grep -c '```' "$MIGRATION" || true)
if [ "$code_blocks" -lt 20 ]; then
  echo "migration: FAIL (only $code_blocks code blocks, expected at least 20)"
  exit 1
fi

# Check that README links to it
if ! grep -q "MIGRATION.md" "$ROOT/README.md"; then
  echo "migration: FAIL (README.md does not link to MIGRATION.md)"
  exit 1
fi

echo "migration: ok ($code_blocks code blocks covering ${required_languages})"
