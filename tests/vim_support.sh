#!/bin/sh
# Verify Vim syntax highlighting support structure

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VIM_DIR="$ROOT/editors/vim"

if [ ! -d "$VIM_DIR" ]; then
  echo "vim-support: FAIL (editors/vim directory not found)"
  exit 1
fi

# Check required files
required_files="syntax/tiq.vim ftdetect/tiq.vim README.md"
missing=""

for file in $required_files; do
  if [ ! -f "$VIM_DIR/$file" ]; then
    missing="$missing $file"
  fi
done

if [ -n "$missing" ]; then
  echo "vim-support: FAIL (missing files:$missing)"
  exit 1
fi

# Check syntax file has required elements
if ! grep -q "syn keyword tiqKeyword" "$VIM_DIR/syntax/tiq.vim"; then
  echo "vim-support: FAIL (syntax file missing keyword definitions)"
  exit 1
fi

if ! grep -q "syn keyword tiqType" "$VIM_DIR/syntax/tiq.vim"; then
  echo "vim-support: FAIL (syntax file missing type definitions)"
  exit 1
fi

if ! grep -q "syn match tiqOperator" "$VIM_DIR/syntax/tiq.vim"; then
  echo "vim-support: FAIL (syntax file missing operator definitions)"
  exit 1
fi

# Check ftdetect recognizes .tiq files
if ! grep -q "\.tiq" "$VIM_DIR/ftdetect/tiq.vim"; then
  echo "vim-support: FAIL (ftdetect does not recognize .tiq files)"
  exit 1
fi

echo "vim-support: ok (syntax and ftdetect validated)"
