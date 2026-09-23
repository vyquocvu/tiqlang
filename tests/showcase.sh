#!/bin/sh
# Verify Showcase document exists and covers key use cases

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SHOWCASE="$ROOT/docs/SHOWCASE.md"

if [ ! -f "$SHOWCASE" ]; then
  echo "showcase: FAIL (docs/SHOWCASE.md not found)"
  exit 1
fi

# Check for key sections
required_sections="Systems Tools Network Services Data Processing Compiler Performance"
missing=""

for section in $required_sections; do
  if ! grep -qi "$section" "$SHOWCASE"; then
    missing="$missing $section"
  fi
done

if [ -n "$missing" ]; then
  echo "showcase: FAIL (missing sections:$missing)"
  exit 1
fi

# Check for code examples
code_blocks=$(grep -c '```' "$SHOWCASE" || true)
if [ "$code_blocks" -lt 5 ]; then
  echo "showcase: FAIL (only $code_blocks code blocks, expected at least 5)"
  exit 1
fi

# Check that README links to it
if ! grep -q "SHOWCASE.md" "$ROOT/README.md"; then
  echo "showcase: FAIL (README.md does not link to SHOWCASE.md)"
  exit 1
fi

echo "showcase: ok ($code_blocks code blocks covering real-world use cases)"
