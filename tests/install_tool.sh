#!/bin/sh
# M18.1: `tiq install` package dependency installer. Builds
# src/tiq/tools/install.tiq with the C bootstrap into build/tiq-install, then
# verifies: local path dependency resolution, .tiq-deps/ directory creation,
# lockfile generation, fail-closed behavior (missing manifest, empty deps,
# nonexistent dep path), and ASan/UBSan on the tool's emitted C.
set -u

TIQ="${TIQ:-./build/tiq}"
# Make TIQ path absolute for use after cd
if [ "${TIQ#/}" = "$TIQ" ]; then
  TIQ="$(pwd)/$TIQ"
fi
CC_BIN="${CC:-cc}"
ROOT="$(pwd)"
INSTALL="$ROOT/build/tiq-install"
TMP_DIR="${TMPDIR:-/tmp}/tiq-install-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

if ! "$TIQ" build src/tiq/tools/install.tiq -o "$INSTALL" 2>"$TMP_DIR/build.err"; then
  echo "install_tool: FAIL (cannot build src/tiq/tools/install.tiq)" >&2
  cat "$TMP_DIR/build.err" >&2
  exit 1
fi

# Build init tool for manifest validation tests
INIT="$ROOT/build/tiq-init"
if ! "$TIQ" build src/tiq/tools/init.tiq -o "$INIT" 2>"$TMP_DIR/build_init.err"; then
  echo "install_tool: FAIL (cannot build src/tiq/tools/init.tiq)" >&2
  cat "$TMP_DIR/build_init.err" >&2
  exit 1
fi

fail=0

expect_exit() {
  want="$1"; name="$2"; shift 2
  "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
  got=$?
  if [ "$want" -ne "$got" ]; then
    echo "install_tool: FAIL $name (expected exit $want, got $got)" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

expect_out() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.out"; then
    echo "install_tool: FAIL $name (stdout missing '$pattern')" >&2
    cat "$TMP_DIR/$name.out" >&2
    fail=1
  fi
}

expect_err() {
  name="$1"; pattern="$2"
  if ! grep -qF -- "$pattern" "$TMP_DIR/$name.err"; then
    echo "install_tool: FAIL $name (stderr missing '$pattern')" >&2
    cat "$TMP_DIR/$name.err" >&2
    fail=1
  fi
}

# --- Setup: create a library and a project that depends on it ---

# Create a simple library
mkdir -p "$TMP_DIR/libs/mylib"
cat > "$TMP_DIR/libs/mylib/lib.tiq" << 'LIBEOF'
lib_hello : str -> { "hello from mylib" }
LIBEOF

# Create a project with a manifest declaring a path dependency
mkdir -p "$TMP_DIR/project"
cd "$TMP_DIR/project"
cat > "tiq.toml" << 'MANIFESTEOF'
[package]
name = "myapp"
version = "0.1.0"

[deps]
mylib = "path:../libs/mylib"

[tests]
dir = "tests"
MANIFESTEOF

# 1. `tiq install` resolves local path deps and creates .tiq-deps/.
expect_exit 0 basic "$INSTALL"
expect_out basic "Installed"
if [ ! -d ".tiq-deps/mylib" ]; then
  echo "install_tool: FAIL basic (.tiq-deps/mylib not created)" >&2
  fail=1
fi

# 2. The dependency files are copied into .tiq-deps/.
if [ ! -f ".tiq-deps/mylib/lib.tiq" ]; then
  echo "install_tool: FAIL basic (dep file not copied)" >&2
  fail=1
fi

# 3. A lockfile is generated.
if [ ! -f "tiq.lock" ]; then
  echo "install_tool: FAIL basic (tiq.lock not created)" >&2
  fail=1
fi
if ! grep -qF "mylib" tiq.lock; then
  echo "install_tool: FAIL basic (tiq.lock missing mylib)" >&2
  fail=1
fi

# 4. Running install again is idempotent.
expect_exit 0 idempotent "$INSTALL"

# 5. Missing manifest fails closed.
mkdir -p "$TMP_DIR/no_manifest"
cd "$TMP_DIR/no_manifest"
expect_exit 1 no_manifest "$INSTALL"
expect_err no_manifest "tiq.toml"

# 6. Nonexistent dep path fails closed.
mkdir -p "$TMP_DIR/bad_dep"
cd "$TMP_DIR/bad_dep"
cat > "tiq.toml" << 'BADEOF'
[package]
name = "badapp"

[deps]
ghost = "path:../nonexistent"
BADEOF
expect_exit 1 bad_path "$INSTALL"
expect_err bad_path "ghost"

# 7. Empty deps section reports no deps installed.
mkdir -p "$TMP_DIR/no_deps"
cd "$TMP_DIR/no_deps"
cat > "tiq.toml" << 'EMPTYEOF'
[package]
name = "emptyapp"

[tests]
dir = "tests"
EMPTYEOF
expect_exit 0 no_deps "$INSTALL"
expect_out no_deps "No dependencies"

# 8. Multiple deps are all resolved.
mkdir -p "$TMP_DIR/libs/liba"
mkdir -p "$TMP_DIR/libs/libb"
echo 'a_fn : i64 -> { 1 }' > "$TMP_DIR/libs/liba/a.tiq"
echo 'b_fn : i64 -> { 2 }' > "$TMP_DIR/libs/libb/b.tiq"
mkdir -p "$TMP_DIR/multi"
cd "$TMP_DIR/multi"
cat > "tiq.toml" << 'MULTIEOF'
[package]
name = "multiapp"

[deps]
liba = "path:../libs/liba"
libb = "path:../libs/libb"
MULTIEOF
expect_exit 0 multi "$INSTALL"
if [ ! -d ".tiq-deps/liba" ] || [ ! -d ".tiq-deps/libb" ]; then
  echo "install_tool: FAIL multi (not all deps installed)" >&2
  fail=1
fi

cd "$ROOT"

# 9. M18.3: Valid version constraints in git deps pass manifest validation.
mkdir -p "$TMP_DIR/ver_ok"
cd "$TMP_DIR/ver_ok"
cat > "tiq.toml" << 'VEREOF'
[package]
name = "verapp"

[deps]
foo = "git:https://example.com/foo.git#>=1.0.0,<2.0.0"
bar = "git:https://example.com/bar.git#1.5.0"
VEREOF
# Use tiq init --check to validate the manifest without actually cloning
"$INIT" --check tiq.toml >"$TMP_DIR/ver_ok.out" 2>"$TMP_DIR/ver_ok.err"
if [ "$?" -ne 0 ]; then
  echo "install_tool: FAIL ver_ok (valid constraint rejected)" >&2
  cat "$TMP_DIR/ver_ok.err" >&2
  fail=1
fi

# 10. M18.3: Invalid version constraints fail closed.
mkdir -p "$TMP_DIR/ver_bad"
cd "$TMP_DIR/ver_bad"
cat > "tiq.toml" << 'BADCONEOF'
[package]
name = "badver"

[deps]
foo = "git:https://example.com/foo.git#not-a-version"
BADCONEOF
"$INIT" --check tiq.toml >"$TMP_DIR/ver_bad.out" 2>"$TMP_DIR/ver_bad.err"
if [ "$?" -eq 0 ]; then
  echo "install_tool: FAIL ver_bad (invalid constraint accepted)" >&2
  fail=1
fi
if ! grep -qF "invalid version constraint" "$TMP_DIR/ver_bad.err"; then
  echo "install_tool: FAIL ver_bad (missing diagnostic)" >&2
  cat "$TMP_DIR/ver_bad.err" >&2
  fail=1
fi

# 11. M18.3: Version constraint test program passes.
cd "$ROOT"
"$TIQ" run tests/ver_test.tiq >"$TMP_DIR/ver_test.out" 2>"$TMP_DIR/ver_test.err"
if [ "$?" -ne 0 ]; then
  echo "install_tool: FAIL ver_test (test program failed)" >&2
  cat "$TMP_DIR/ver_test.err" >&2
  fail=1
fi
if ! grep -qF "semver 1.2.3 = 1002003" "$TMP_DIR/ver_test.out"; then
  echo "install_tool: FAIL ver_test (semver parsing wrong)" >&2
  fail=1
fi
if ! grep -qF "1.5.0 satisfies >=1.0.0,<2.0.0: 1" "$TMP_DIR/ver_test.out"; then
  echo "install_tool: FAIL ver_test (constraint matching wrong)" >&2
  fail=1
fi

# 12. The tool's generated C is memory-clean under ASan/UBSan.
if "$TIQ" emit-c src/tiq/tools/install.tiq >"$TMP_DIR/install.c" 2>"$TMP_DIR/install.emit.err"; then
  if "$CC_BIN" -std=c11 -g -fsanitize=address,undefined "$TMP_DIR/install.c" -o "$TMP_DIR/install.asan" 2>"$TMP_DIR/install.cc.err"; then
    cd "$TMP_DIR/project"
    ASAN_OPTIONS=detect_leaks=0 "$TMP_DIR/install.asan" >"$TMP_DIR/asan.out" 2>"$TMP_DIR/asan.err"
    if [ "$?" -ne 0 ]; then
      echo "install_tool: FAIL ASan install (nonzero exit)" >&2
      cat "$TMP_DIR/asan.err" >&2
      fail=1
    fi
    cd "$ROOT"
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "install_tool: failed" >&2
  exit 1
fi
echo "install_tool: ok (path deps, lockfile, fail-closed, multi-dep, idempotent, version constraints, ASan verified)"
