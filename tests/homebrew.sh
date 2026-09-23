#!/bin/sh
# Verify Homebrew formula structure

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HOMEBREW_DIR="$ROOT/packaging/homebrew"

if [ ! -d "$HOMEBREW_DIR" ]; then
  echo "homebrew: FAIL (packaging/homebrew directory not found)"
  exit 1
fi

# Check required files
if [ ! -f "$HOMEBREW_DIR/tiq.rb" ]; then
  echo "homebrew: FAIL (tiq.rb formula not found)"
  exit 1
fi

if [ ! -f "$HOMEBREW_DIR/README.md" ]; then
  echo "homebrew: FAIL (README.md not found)"
  exit 1
fi

# Check formula has required elements
if ! grep -q "class Tiq < Formula" "$HOMEBREW_DIR/tiq.rb"; then
  echo "homebrew: FAIL (formula missing class definition)"
  exit 1
fi

if ! grep -q "def install" "$HOMEBREW_DIR/tiq.rb"; then
  echo "homebrew: FAIL (formula missing install method)"
  exit 1
fi

if ! grep -q "test do" "$HOMEBREW_DIR/tiq.rb"; then
  echo "homebrew: FAIL (formula missing test block)"
  exit 1
fi

if ! grep -q 'system "make"' "$HOMEBREW_DIR/tiq.rb"; then
  echo "homebrew: FAIL (formula does not build with make)"
  exit 1
fi

echo "homebrew: ok (formula structure validated)"
