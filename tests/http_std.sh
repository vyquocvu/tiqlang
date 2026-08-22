#!/bin/sh
# M19.3: HTTP/1.1 client & server tests (std/http.tiq)
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-http-std-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "http_std: FAIL $1" >&2
  shift
  for f in "$@"; do
    echo "--- $f" >&2
    cat "$f" >&2 || true
  done
  exit 1
}

# Test 1: Check syntax
if ! "$TIQ" check std/http.tiq > /dev/null 2>&1; then
  fail "check std/http.tiq failed"
fi

if ! "$TIQ" check examples/http_client.tiq > /dev/null 2>&1; then
  fail "check examples/http_client.tiq failed"
fi

# Test 2: Run examples/http_client.tiq
OUT_FILE="$TMP_DIR/demo.out"
ERR_FILE="$TMP_DIR/demo.err"
EXP_FILE="$TMP_DIR/demo.expected"

if ! "$TIQ" run examples/http_client.tiq > "$OUT_FILE" 2> "$ERR_FILE"; then
  fail "run examples/http_client.tiq failed" "$ERR_FILE"
fi

cat > "$EXP_FILE" << 'EOF'
Built response len: 116
Parsed HTTP status: 200
Parsed HTTP body: {"status":"ok","code":100}
Decoded chunked body: MozillaDeveloperNetwork
HTTP client demo completed successfully.
EOF

if ! cmp -s "$EXP_FILE" "$OUT_FILE"; then
  fail "http demo output mismatch" "$EXP_FILE" "$OUT_FILE"
fi

# Test 3: Loopback HTTP Client & Server test
cat > "$TMP_DIR/http_server_test.tiq" << 'EOF'
import "std/net.tiq"
import "std/http.tiq"

// 1. Listen on ephemeral port
srv_fd = net_listen(0)
port = net_port(srv_fd)

// 2. Client GET request
cli_fd = net_connect(port)
conn_fd = net_accept(srv_fd)

req_wire = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
net_send(cli_fd, req_wire)

srv_req = net_recv(conn_fd)
srv_resp = http_server_response(200, "text/plain", "Hello from Tiq HTTP!")
net_send(conn_fd, srv_resp)

cli_resp = net_recv(cli_fd)
status = http_response_status(cli_resp)
body = http_response_body(cli_resp)

print(str_cat("Client got status: ", int_str(status)))
print(str_cat("Client got body: ", body))

net_close(cli_fd)
net_close(conn_fd)
net_close(srv_fd)
EOF

LOOP_OUT="$TMP_DIR/loop.out"
LOOP_ERR="$TMP_DIR/loop.err"
LOOP_EXP="$TMP_DIR/loop.expected"

if ! "$TIQ" run "$TMP_DIR/http_server_test.tiq" > "$LOOP_OUT" 2> "$LOOP_ERR"; then
  fail "run http_server_test.tiq failed" "$LOOP_ERR"
fi

cat > "$LOOP_EXP" << 'EOF'
Client got status: 200
Client got body: Hello from Tiq HTTP!
EOF

if ! cmp -s "$LOOP_EXP" "$LOOP_OUT"; then
  fail "http server test output mismatch" "$LOOP_EXP" "$LOOP_OUT"
fi

echo "http_std: ok"
