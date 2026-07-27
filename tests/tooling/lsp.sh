#!/bin/sh
# Golden JSON-RPC transcript for `tiq lsp` (plan 5.1).
# Requests are framed with Content-Length headers and piped to the server;
# the captured response stream must match the golden transcript byte for
# byte, so hover/definition/semanticTokens answers are pinned to real
# symbol data from the lexer+parser+semantic front end.

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-lsp-test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# Frame one JSON-RPC message (ASCII only, so ${#1} counts bytes).
frame() {
    printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"
}

# Document under test (version 1):
#   line 0: n = 41
#   line 1: max a b -> a > b ? a : b
#   line 2: !max(n, 1)
{
    frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///ws"}}'
    frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///test.tiq","languageId":"tiq","version":1,"text":"n = 41\nmax a b -> a > b ? a : b\n!max(n, 1)\n"}}}'
    frame '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///test.tiq"},"position":{"line":2,"character":5}}}'
    frame '{"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///test.tiq"},"position":{"line":2,"character":1}}}'
    frame '{"jsonrpc":"2.0","id":4,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///test.tiq"},"position":{"line":2,"character":5}}}'
    frame '{"jsonrpc":"2.0","id":5,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///test.tiq"}}}'
    frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///other.tiq"},"position":{"line":0,"character":0}}}'
    frame '{"jsonrpc":"2.0","id":7,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit","params":null}'
} > "$TMP/requests.bin"

# Golden responses. Hover: markdown code block with `name: type` from the
# semantic checker (max's params/return stay unknown under current local
# inference, which the transcript pins honestly). Definition: declaration
# token of `n` on line 0. Semantic tokens: LSP delta encoding over legend
# [keyword,variable,number,string,operator]. Unopened uri: null (fail
# closed). publishDiagnostics echoes the stored document version.
{
    frame '{"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":1,"hoverProvider":true,"definitionProvider":true,"semanticTokensProvider":{"legend":{"tokenTypes":["keyword","variable","number","string","operator"],"tokenModifiers":[]},"full":true}},"serverInfo":{"name":"tiq","version":"0.1.0"}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///test.tiq","version":1,"diagnostics":[]}}'
    frame '{"jsonrpc":"2.0","id":2,"result":{"contents":{"kind":"markdown","value":"```tiq\nn: int\n```"},"range":{"start":{"line":2,"character":5},"end":{"line":2,"character":6}}}}'
    frame '{"jsonrpc":"2.0","id":3,"result":{"contents":{"kind":"markdown","value":"```tiq\nmax: fn(2) -> unknown\n```"},"range":{"start":{"line":2,"character":1},"end":{"line":2,"character":4}}}}'
    frame '{"jsonrpc":"2.0","id":4,"result":{"uri":"file:///test.tiq","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}}}'
    frame '{"jsonrpc":"2.0","id":5,"result":{"data":[0,0,1,1,0,0,2,1,4,0,0,2,2,2,0,1,0,3,1,0,0,4,1,1,0,0,2,1,1,0,0,2,2,4,0,0,3,1,1,0,0,2,1,4,0,0,2,1,1,0,0,2,1,4,0,0,2,1,1,0,0,2,1,4,0,0,2,1,1,0,1,0,1,4,0,0,1,3,1,0,0,4,1,1,0,0,3,1,2,0]}}'
    frame '{"jsonrpc":"2.0","id":6,"result":null}'
    frame '{"jsonrpc":"2.0","id":7,"result":null}'
} > "$TMP/expected.bin"

"$TIQ" lsp < "$TMP/requests.bin" > "$TMP/actual.bin"

if cmp -s "$TMP/expected.bin" "$TMP/actual.bin"; then
    echo "lsp: ok"
else
    echo "lsp: transcript mismatch"
    echo "--- expected ---"
    cat -v "$TMP/expected.bin"
    echo ""
    echo "--- actual ---"
    cat -v "$TMP/actual.bin"
    echo ""
    exit 1
fi
