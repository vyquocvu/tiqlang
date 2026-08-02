#!/bin/sh
# M14.4-T1: `tiq init` and package manifest harness. Builds
# src/tiq/tools/init.tiq with the C bootstrap into build/tiq-init, then
# verifies manifest creation (default tiq.toml and named *.tiq.toml) with the
# deterministic template, fail-closed behavior (existing file, invalid name,
# unknown options), --check validation of existing manifests (valid passes,
# bad version / missing name / unknown key / unknown section / unreadable all
# fail closed with a located diagnostic on stderr), and ASan/UBSan on the
# tool's emitted C.
set -u

TIQ="${TIQ:-./build/tiq}"
CC_BIN="${CC:-cc}"
ROOT="$(pwd)"
INIT="$ROOT/build/tiq-init"
TMP_DIR="${TMPDIR:-/tmp}/tiq-init-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/init.tiq -o "$INIT" 2>"$TMP_DIR/build.err"; then
  echo "init_tool: FAIL (cannot build src/tiq/tools/init.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

fail=0

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "init_tool: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

expect_out() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.out"; then
    echo "init_tool: FAIL $name (stdout missing '$pattern')" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_err() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.err"; then
    echo "init_tool: FAIL $name (stderr missing '$pattern')" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

mkdir -p "$TMP_DIR/pkg"
cd "$TMP_DIR/pkg"

# 1. `tiq init` creates tiq.toml with the deterministic template.
expect_exit 0 default "$INIT"
expect_out default "Created tiq.toml"
if [ ! -f "tiq.toml" ]; then
  echo "init_tool: FAIL default (tiq.toml not created)" >&2
  fail=1
fi
grep -qF '[package]' tiq.toml || { echo "init_tool: FAIL default (no [package])" >&2; fail=1; }
grep -qF 'name = "my-package"' tiq.toml || { echo "init_tool: FAIL default (bad name)" >&2; fail=1; }
grep -qF 'version = "0.1.0"' tiq.toml || { echo "init_tool: FAIL default (bad version)" >&2; fail=1; }
grep -qF '[tests]' tiq.toml || { echo "init_tool: FAIL default (no [tests])" >&2; fail=1; }
grep -qF 'dir = "tests"' tiq.toml || { echo "init_tool: FAIL default (no tests dir)" >&2; fail=1; }

# 2. `tiq init <name>` creates <name>.tiq.toml with that package name.
expect_exit 0 named "$INIT" mypkg
expect_out named "Created mypkg.tiq.toml"
if [ ! -f "mypkg.tiq.toml" ]; then
  echo "init_tool: FAIL named (mypkg.tiq.toml not created)" >&2
  fail=1
fi
grep -qF 'name = "mypkg"' mypkg.tiq.toml || { echo "init_tool: FAIL named (bad name)" >&2; fail=1; }

# 3. An existing manifest is not clobbered (fail closed, exit 1).
expect_exit 1 existing "$INIT"
expect_err existing "tiq.toml already exists"
if [ "$(cat tiq.toml | grep -c 'name = ')" -ne 1 ]; then
  echo "init_tool: FAIL existing (manifest clobbered)" >&2
  fail=1
fi

# 4. Invalid package names are rejected before any file is written.
expect_exit 2 bad_name "$INIT" "has space"
expect_exit 2 bad_name2 "$INIT" ".."
expect_exit 2 bad_name3 "$INIT" "a/b"

# 5. --check passes on a valid manifest.
expect_exit 0 check_valid "$INIT" --check tiq.toml

# 6. A bad version fails closed with a located diagnostic.
cat > "badver.tiq.toml" << 'EOF'
[package]
name = "x"
version = "1.2"
EOF
expect_exit 1 check_badver "$INIT" --check badver.tiq.toml
expect_err check_badver "badver.tiq.toml:3: error[E30]:"

# 7. A missing name fails closed.
cat > "noname.tiq.toml" << 'EOF'
[package]
version = "0.1.0"
EOF
expect_exit 1 check_noname "$INIT" --check noname.tiq.toml
expect_err check_noname "noname.tiq.toml:"

# 8. An unknown key fails closed.
cat > "badkey.tiq.toml" << 'EOF'
[package]
name = "x"
frobnicate = "1"
EOF
expect_exit 1 check_badkey "$INIT" --check badkey.tiq.toml
expect_err check_badkey "badkey.tiq.toml:"

# 9. An unknown section fails closed.
cat > "badsec.tiq.toml" << 'EOF'
[package]
name = "x"
[wat]
EOF
expect_exit 1 check_badsec "$INIT" --check badsec.tiq.toml
expect_err check_badsec "badsec.tiq.toml:"

# 10. An unreadable/missing manifest fails closed.
expect_exit 1 check_missing "$INIT" --check no-such.tiq.toml
expect_err check_missing "cannot read"

# 11. Usage errors exit 2.
expect_exit 2 unknown_flag "$INIT" --frobnicate
expect_exit 2 check_no_arg "$INIT" --check
expect_exit 2 check_with_name "$INIT" --check tiq.toml extra

cd "$ROOT"

# 12. The tool's generated C is memory-clean under ASan/UBSan.
if "$TIQ" emit-c src/tiq/tools/init.tiq >"$TMP_DIR/init.c" 2>"$TMP_DIR/init.emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/init.c" -o "$TMP_DIR/init.asan" 2>"$TMP_DIR/init.cc.err"; then
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/init.asan" --check "$TMP_DIR/pkg/tiq.toml" >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    if [ "$?" -ne 0 ]; then
      echo "init_tool: FAIL ASan init (nonzero exit on valid manifest)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/init.asan" --check "$TMP_DIR/pkg/badver.tiq.toml" >"$TMP_DIR/asan2.out" 2>"$TMP_DIR/asan2.err"
    if [ "$?" -eq 0 ]; then
      echo "init_tool: FAIL ASan init (bad manifest accepted)" >&2
      fail=1
    fi
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "init_tool: failed" >&2
  exit 1
fi
echo "init_tool: ok (create default/named, fail-closed, --check valid/invalid, usage, ASan verified)"
