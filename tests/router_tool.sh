#!/bin/sh
# M21.3-T3: routed HTTP service dogfood harness. The service is authored in
# Tiq and exercises bounded request parsing, static/parameter routes, method
# dispatch, body echoing, and fail-closed HTTP errors.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d /tmp/tiq-router-test.XXXXXX)"
PORT="${TIQ_ROUTER_TEST_PORT:-19106}"
BIN="$ROOT/build/tiq-router"
PID=""

cleanup() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

fail() {
  echo "router_tool: FAIL $*" >&2
  [ -f "$TMP_DIR/server.err" ] && cat "$TMP_DIR/server.err" >&2
  exit 1
}

"$TIQ" build src/tiq/tools/router.tiq -o "$BIN" 2>"$TMP_DIR/build.err" || {
  cat "$TMP_DIR/build.err" >&2
  fail "cannot build router"
}

set +e
"$BIN" >"$TMP_DIR/usage.out" 2>"$TMP_DIR/usage.err"
code=$?
set -e
[ "$code" -eq 2 ] || fail "missing port exited $code, expected 2"
grep -q "usage" "$TMP_DIR/usage.err" || fail "missing usage diagnostic"

set +e
"$BIN" invalid >"$TMP_DIR/port.out" 2>"$TMP_DIR/port.err"
code=$?
set -e
[ "$code" -eq 2 ] || fail "invalid port exited $code, expected 2"
grep -q "1..65535" "$TMP_DIR/port.err" || fail "missing invalid-port diagnostic"

"$BIN" "$PORT" >"$TMP_DIR/server.out" 2>"$TMP_DIR/server.err" &
PID=$!
i=0
while [ "$i" -lt 50 ]; do
  curl -fsS "http://127.0.0.1:$PORT/health" >"$TMP_DIR/ready" 2>/dev/null && break
  i=$((i + 1))
  sleep 0.1
done
[ "$(cat "$TMP_DIR/ready" 2>/dev/null || true)" = "ok" ] || fail "server did not become ready"

body="$(curl -fsS "http://127.0.0.1:$PORT/hello/Tiq")"
[ "$body" = "hello, Tiq" ] || fail "parameter route returned '$body'"

body="$(curl -fsS -X POST --data-binary 'dogfood body' "http://127.0.0.1:$PORT/echo")"
[ "$body" = "dogfood body" ] || fail "echo route returned '$body'"

code="$(curl -sS -o "$TMP_DIR/not-found" -w '%{http_code}' "http://127.0.0.1:$PORT/missing")"
[ "$code" = 404 ] || fail "unknown route returned $code, expected 404"

code="$(curl -sS -o "$TMP_DIR/method" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/health")"
[ "$code" = 405 ] || fail "wrong method returned $code, expected 405"

# Send an incomplete request then close the write side. The service must return
# a complete 400 response rather than treating the partial input as a route.
python3 - "$PORT" >"$TMP_DIR/malformed" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")
s.shutdown(socket.SHUT_WR)
data = b""
while True:
    part = s.recv(4096)
    if not part:
        break
    data += part
sys.stdout.buffer.write(data)
PY
grep -q "400 Bad Request" "$TMP_DIR/malformed" || fail "incomplete request did not return 400"

python3 - "$PORT" >"$TMP_DIR/oversized" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 70000\r\n\r\n")
s.shutdown(socket.SHUT_WR)
data = b""
while True:
    part = s.recv(4096)
    if not part:
        break
    data += part
sys.stdout.buffer.write(data)
PY
grep -q "413 Payload Too Large" "$TMP_DIR/oversized" || fail "oversized request did not return 413"

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""

"$TIQ" emit-c src/tiq/tools/router.tiq >"$TMP_DIR/router.c" 2>"$TMP_DIR/emit.err" || fail "emit-c failed"
"$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/router.c" -o "$TMP_DIR/router.asan" 2>"$TMP_DIR/cc.err" || fail "sanitizer compile failed"
set +e
ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/router.asan" >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
code=$?
set -e
[ "$code" -eq 2 ] || fail "sanitized usage path exited $code, expected 2"
! grep -q "runtime error:" "$TMP_DIR/asan.err" || fail "UBSan reported a runtime error"
! grep -q "ERROR: AddressSanitizer" "$TMP_DIR/asan.err" || fail "ASan reported an error"

echo "router_tool: ok (static/parameter/echo routes, 400/404/405, bounds, ASan/UBSan)"
