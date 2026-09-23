#!/bin/sh
# Verify Getting Started guide exists and covers key onboarding steps

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GUIDE="$ROOT/docs/GETTING_STARTED.md"

if [ ! -f "$GUIDE" ]; then
  echo "getting-started: FAIL (docs/GETTING_STARTED.md not found)"
  exit 1
fi

# Check for key sections
required_sections="Prerequisites Installation First Program Variables Functions Loops"
missing=""

for section in $required_sections; do
  if ! grep -qi "$section" "$GUIDE"; then
    missing="$missing $section"
  fi
done

if [ -n "$missing" ]; then
  echo "getting-started: FAIL (missing sections:$missing)"
  exit 1
fi

# Check for code examples
code_blocks=$(grep -c '```tiq' "$GUIDE" || true)
if [ "$code_blocks" -lt 5 ]; then
  echo "getting-started: FAIL (only $code_blocks Tiq code examples, expected at least 5)"
  exit 1
fi

# Check that README links to it
if ! grep -q "GETTING_STARTED.md" "$ROOT/README.md"; then
  echo "getting-started: FAIL (README.md does not link to GETTING_STARTED.md)"
  exit 1
fi

echo "getting-started: ok ($code_blocks code examples covering onboarding)"
