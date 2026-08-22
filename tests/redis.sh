#!/bin/sh
# M19.6: Redis connector tests (std/redis.tiq)
set -eu

TIQ="${TIQ:-./build/tiq}"
TMP_DIR="${TMPDIR:-/tmp}/tiq-redis-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

fail() {
  echo "redis: FAIL $1" >&2
  shift
  for f in "$@"; do
    echo "--- $f" >&2
    cat "$f" >&2 || true
  done
  exit 1
}

# Test 1: Check syntax
if ! "$TIQ" check std/redis.tiq > /dev/null 2>&1; then
  fail "check std/redis.tiq failed"
fi

if ! "$TIQ" check examples/redis_demo.tiq > /dev/null 2>&1; then
  fail "check examples/redis_demo.tiq failed"
fi

# Test 2: Run examples/redis_demo.tiq end-to-end
EXPECTED="PING command formatted: len=14
SET command formatted: len=45
Parsed simple string: PONG
Parsed bulk string: hello world
Parsed integer: 42
Parsed nil bulk string: ''
Redis demo completed successfully."

OUT_FILE="$TMP_DIR/demo.out"
ERR_FILE="$TMP_DIR/demo.err"
EXP_FILE="$TMP_DIR/demo.expected"

if ! "$TIQ" run examples/redis_demo.tiq > "$OUT_FILE" 2> "$ERR_FILE"; then
  fail "run examples/redis_demo.tiq failed" "$ERR_FILE"
fi

printf '%s\n' "$EXPECTED" > "$EXP_FILE"
if ! cmp -s "$EXP_FILE" "$OUT_FILE"; then
  fail "redis demo output mismatch" "$EXP_FILE" "$OUT_FILE"
fi

# Test 3: Integration test with mock Redis server in Tiq
cat > "$TMP_DIR/mock_server_test.tiq" << 'EOF'
import "std/net.tiq"
import "std/redis.tiq"

// Start mock server on loopback port
srv_fd = net_listen(0)
port = net_port(srv_fd)

// Connect client
cli_fd = redis_connect(port)
conn_fd = net_accept(srv_fd)

// 1. Test PING
// Client sends PING
cmd = redis_cmd_1("PING")
net_send(cli_fd, cmd)

// Server receives and sends +PONG\r\n
req = net_recv(conn_fd)
net_send(conn_fd, "+PONG\r\n")

// Client receives
resp = net_recv(cli_fd)
pong = redis_parse_response(resp)
print(str_cat("Client received ping reply: ", pong))

// 2. Test SET & GET
// Client sends SET key val
cmd_set = redis_cmd_3("SET", "user", "alice")
net_send(cli_fd, cmd_set)
req2 = net_recv(conn_fd)
net_send(conn_fd, "+OK\r\n")
resp2 = net_recv(cli_fd)
set_res = redis_parse_response(resp2)
print(str_cat("Client set ok: ", set_res))

// Client sends GET key
cmd_get = redis_cmd_2("GET", "user")
net_send(cli_fd, cmd_get)
req3 = net_recv(conn_fd)
net_send(conn_fd, "$5\r\nalice\r\n")
resp3 = net_recv(cli_fd)
get_res = redis_parse_response(resp3)
print(str_cat("Client get val: ", get_res))

// Close sockets
redis_close(cli_fd)
net_close(conn_fd)
net_close(srv_fd)
print("Mock Redis test passed.")
EOF

MOCK_OUT="$TMP_DIR/mock.out"
MOCK_ERR="$TMP_DIR/mock.err"
MOCK_EXP="$TMP_DIR/mock.expected"

if ! "$TIQ" run "$TMP_DIR/mock_server_test.tiq" > "$MOCK_OUT" 2> "$MOCK_ERR"; then
  fail "run mock_server_test.tiq failed" "$MOCK_ERR"
fi

cat > "$MOCK_EXP" << 'EOF'
Client received ping reply: PONG
Client set ok: OK
Client get val: alice
Mock Redis test passed.
EOF

if ! cmp -s "$MOCK_EXP" "$MOCK_OUT"; then
  fail "mock server test output mismatch" "$MOCK_EXP" "$MOCK_OUT"
fi

echo "redis: ok"
