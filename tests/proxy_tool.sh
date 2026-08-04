#!/bin/sh
# M21.3-T1: loopback HTTP reverse proxy test harness (dogfooding: real
# infrastructure written in Tiq). Builds src/tiq/tools/proxy.tiq plus a tiny
# Tiq upstream server, then verifies:
#   - GET requests pass through with the upstream body and 200 status
#   - POST bodies are forwarded (Content-Length-aware) and echoed back
#   - a dead upstream yields a fail-closed 502
#   - usage errors (missing/non-numeric ports) exit 2
#   - the proxy's emitted C is ASan/UBSan clean on the usage path
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TIQ="${TIQ:-./build/tiq}"
if [ "${TIQ#/}" = "$TIQ" ]; then
  TIQ="$(pwd)/$TIQ"
fi
CC_BIN="${CC:-cc}"

TMP_DIR=$(mktemp -d /tmp/tiq-proxy-test.XXXXXX)
fail=0

UP_PORT=19095
PROXY_PORT=19096
DEAD_PORT=19097

PROXY="$ROOT/build/tiq-proxy"
UPSTREAM_BIN="$TMP_DIR/upstream"
PROXY_PID=""
UP_PID=""

cleanup() {
  [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
  [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/proxy.tiq -o "$PROXY" 2>"$TMP_DIR/build_proxy.err"; then
  echo "proxy_tool: FAIL (cannot build src/tiq/tools/proxy.tiq)" >&2
  cat "$TMP_DIR/build_proxy.err" >&2
  exit 1
fi

# Tiny upstream server: GET answers "upstream:<path>"; POST echoes the body.
cat >"$TMP_DIR/upstream.tiq" <<EOF
import "std/net.tiq"

srv <- net_listen($UP_PORT)
?[srv < 0] { proc_exit(1) }
[1 == 1] {
    conn <- net_accept(srv)
    ?[conn < 0] { break }
    req <- net_recv(conn)
    ?[len(req) == 0] { net_close(conn); break }
    method <- http_method(req)
    path <- http_path(req)
    body <- ""
    bn <- len(req)
    bi <- 0
    ?[str_eq(method, "POST")] {
        [bi + 3 < bn] {
            ?[str_sub_code(req, bi) == 13 && str_sub_code(req, bi + 1) == 10 && str_sub_code(req, bi + 2) == 13 && str_sub_code(req, bi + 3) == 10] {
                body <- str_sub(req, bi + 4, bn)
                bi <- bn
            }
            bi <- bi + 1
        }
    }
    resp_body <- str_cat("upstream:", path)
    ?[str_eq(method, "POST")] { resp_body <- body }
    sb <- str_buf_new()
    str_buf_append(sb, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ")
    str_buf_append(sb, int_str(len(resp_body)))
    str_buf_append(sb, "\r\nConnection: close\r\n\r\n")
    str_buf_append(sb, resp_body)
    net_send(conn, str_buf_to_str(sb))
    net_close(conn)
}
0
EOF
if ! "$TIQ" build "$TMP_DIR/upstream.tiq" -o "$UPSTREAM_BIN" 2>"$TMP_DIR/build_up.err"; then
  echo "proxy_tool: FAIL (cannot build upstream fixture)" >&2
  cat "$TMP_DIR/build_up.err" >&2
  exit 1
fi

# 1. Missing arguments fail closed with usage (exit 2).
echo "=== 1. Usage: missing args ==="
set +e
"$PROXY" >"$TMP_DIR/u1.out" 2>"$TMP_DIR/u1.err"
code=$?
set -e
if [ "$code" -ne 2 ] || ! grep -q "usage" "$TMP_DIR/u1.err"; then
  echo "proxy_tool: FAIL usage (exit $code, stderr: $(cat "$TMP_DIR/u1.err"))" >&2
  fail=1
fi

# 2. Non-numeric ports fail closed (exit 2).
echo "=== 2. Usage: bad port ==="
set +e
"$PROXY" abc "$UP_PORT" >"$TMP_DIR/u2.out" 2>"$TMP_DIR/u2.err"
code=$?
set -e
if [ "$code" -ne 2 ]; then
  echo "proxy_tool: FAIL bad port (exit $code)" >&2
  fail=1
fi

# Start upstream + proxy.
"$UPSTREAM_BIN" &
UP_PID=$!
"$PROXY" "$PROXY_PORT" "$UP_PORT" >"$TMP_DIR/proxy.out" 2>"$TMP_DIR/proxy.err" &
PROXY_PID=$!
sleep 1

# 3. GET passes through with the upstream body.
echo "=== 3. GET passthrough ==="
body=$(curl -s "http://127.0.0.1:$PROXY_PORT/hello/world")
if [ "$body" != "upstream:/hello/world" ]; then
  echo "proxy_tool: FAIL GET passthrough (got: $body)" >&2
  cat "$TMP_DIR/proxy.err" >&2
  fail=1
fi

# 4. GET status line comes from the upstream (200).
echo "=== 4. Status passthrough ==="
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PROXY_PORT/x")
if [ "$code" != "200" ]; then
  echo "proxy_tool: FAIL status (got: $code)" >&2
  fail=1
fi

# 5. POST body is forwarded (Content-Length-aware) and echoed.
echo "=== 5. POST body passthrough ==="
body=$(curl -s -X POST --data-binary 'hello-proxy-body' "http://127.0.0.1:$PROXY_PORT/echo")
if [ "$body" != "hello-proxy-body" ]; then
  echo "proxy_tool: FAIL POST passthrough (got: $body)" >&2
  cat "$TMP_DIR/proxy.err" >&2
  fail=1
fi

# Stop the proxy; the 502 case needs a proxy with a dead upstream.
kill "$PROXY_PID" 2>/dev/null || true
wait "$PROXY_PID" 2>/dev/null || true
PROXY_PID=""

"$PROXY" "$PROXY_PORT" "$DEAD_PORT" >"$TMP_DIR/proxy2.out" 2>"$TMP_DIR/proxy2.err" &
PROXY_PID=$!
sleep 1

# 6. Dead upstream yields a fail-closed 502.
echo "=== 6. Dead upstream -> 502 ==="
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PROXY_PORT/none")
if [ "$code" != "502" ]; then
  echo "proxy_tool: FAIL 502 (got: $code)" >&2
  cat "$TMP_DIR/proxy2.err" >&2
  fail=1
fi

kill "$PROXY_PID" 2>/dev/null || true
wait "$PROXY_PID" 2>/dev/null || true
PROXY_PID=""

# 7. The proxy's emitted C is ASan/UBSan clean on the usage path.
echo "=== 7. ASan/UBSan ==="
if "$TIQ" emit-c src/tiq/tools/proxy.tiq >"$TMP_DIR/proxy.c" 2>"$TMP_DIR/emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/proxy.c" -o "$TMP_DIR/proxy.asan" 2>"$TMP_DIR/cc.err"; then
    set +e
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/proxy.asan" >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    code=$?
    set -e
    if [ "$code" -ne 2 ]; then
      echo "proxy_tool: FAIL ASan usage path (exit $code)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
  else
    echo "proxy_tool: FAIL ASan compile" >&2
    cat "$TMP_DIR/cc.err" >&2
    fail=1
  fi
else
  echo "proxy_tool: FAIL emit-c" >&2
  cat "$TMP_DIR/emit.err" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "proxy_tool: failed" >&2
  exit 1
fi
echo "proxy_tool: ok (GET/POST passthrough, 502 fail-closed, usage, ASan)"
