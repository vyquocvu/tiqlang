#!/bin/sh
# M14.6-T1: LSP server harness. Builds src/tiq/tools/lsp.tiq with the C
# bootstrap into build/tiq-lsp, then verifies fail-closed on empty input,
# initialize handshake, hover, definition, semantic tokens, and shutdown.
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
ROOT="$(pwd)"
LSP="$ROOT/build/tiq-lsp"
TMP_DIR="${TMPDIR:-/tmp}/tiq-lsp-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/lsp.tiq -o "$LSP" 2>"$TMP_DIR/build.err"; then
  echo "lsp_tool: FAIL (cannot build src/tiq/tools/lsp.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0

# Helper: build a Content-Length framed message stream from JSON bodies.
# Usage: lsp_send name msg1 [msg2 ...]
lsp_send() {
  name="$1"; shift
  : > "$TMP_DIR/$name.input"
  for msg in "$@"; do
    len=$(printf '%s' "$msg" | wc -c | tr -d ' ')
    printf 'Content-Length: %s\r\n\r\n%s' "$len" "$msg" >> "$TMP_DIR/$name.input"
  done
}

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "lsp_tool: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

expect_out() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.out"; then
    echo "lsp_tool: FAIL $name (stdout missing '$pattern')" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_no_out() {
  name="$1"
  if [ -s "$TMP_DIR/$name.out" ]; then
    echo "lsp_tool: FAIL $name (expected empty stdout)" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

# 1. Empty input: exit 1, no output (fail closed).
printf '' > "$TMP_DIR/empty.input"
expect_exit 1 empty "$LSP" < "$TMP_DIR/empty.input"
expect_no_out empty

# 2. Initialize handshake.
lsp_send init \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
expect_exit 0 init "$LSP" < "$TMP_DIR/init.input"
expect_out init '"jsonrpc":"2.0","id":1'
expect_out init '"capabilities"'
expect_out init '"hoverProvider":true'
expect_out init '"definitionProvider":true'
expect_out init '"semanticTokensProvider"'
expect_out init '"serverInfo":{"name":"tiq","version":"0.1.0"}'

# 3. Initialize + shutdown.
lsp_send shutdown \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}}'
expect_exit 0 shutdown "$LSP" < "$TMP_DIR/shutdown.input"
expect_out shutdown '"id":2,"result":null}'

# 4. Initialize + didOpen + hover.
lsp_send hover \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t.tiq","version":1,"text":"x <- 42"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///t.tiq"},"position":{"line":0,"character":0}}}' \
  '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}'
expect_exit 0 hover "$LSP" < "$TMP_DIR/hover.input"
expect_out hover '"id":2,"result":{"contents"'
expect_out hover '"kind":"markdown"'

# 5. Hover on unknown document returns null.
lsp_send hover_null \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///nope.tiq"},"position":{"line":0,"character":0}}}' \
  '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}'
expect_exit 0 hover_null "$LSP" < "$TMP_DIR/hover_null.input"
expect_out hover_null '"id":2,"result":null}'

# 6. Initialize + didOpen + semanticTokens/full.
lsp_send semtok \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t.tiq","version":1,"text":"x <- 42"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///t.tiq"}}}' \
  '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}'
expect_exit 0 semtok "$LSP" < "$TMP_DIR/semtok.input"
expect_out semtok '"id":2,"result":{"data":['

# 7. Definition returns null (stub).
lsp_send defn \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///t.tiq"},"position":{"line":0,"character":0}}}' \
  '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}'
expect_exit 0 defn "$LSP" < "$TMP_DIR/defn.input"
expect_out defn '"id":2,"result":null}'

# 8. ASan/UBSan check on the emitted C.
if "$TIQ" emit-c src/tiq/tools/lsp.tiq >"$TMP_DIR/lsp.c" 2>"$TMP_DIR/lsp.emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/lsp.c" -o "$TMP_DIR/lsp.asan" 2>"$TMP_DIR/lsp.cc.err"; then
    lsp_send asan_init \
      '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
      '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}}'
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/lsp.asan" < "$TMP_DIR/asan_init.input" >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    if [ "$?" -ne 0 ]; then
      echo "lsp_tool: FAIL ASan (initialize+shutdown exit nonzero)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "lsp_tool: failed" >&2
  exit 1
fi
echo "lsp_tool: ok (empty, initialize, shutdown, hover, hover-null, semtok, definition, ASan verified)"
