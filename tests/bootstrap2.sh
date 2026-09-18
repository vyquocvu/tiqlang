#!/bin/sh
# M13-S3: the Tiq-to-Tiq bootstrap chain.
#
# tests/bootstrap.sh proves the self-hosted emitter is a convergent fixed
# point, but every binary it builds is linked by the host C compiler and no
# Tiq-built binary has ever compiled a Tiq program. This harness closes that
# gap exactly once: the C bootstrap seeds stage1, and from there every stage is
# built by the Tiq-built driver in front of it.
#
# Sequence:
#   seed:    ./build/tiq build src/tiq/tiq_main.tiq -o build/tiqb2-stage1
#            (the ONE unavoidable use of the C bootstrap; it is never reused)
#   stage 1: build/tiqb2-stage1 build src/tiq/tiq_main.tiq -o build/tiqb2-stage2
#            <-- a binary produced by Tiq compiling the driver source itself
#   stage 2: build/tiqb2-stage2 build src/tiq/tiq_main.tiq -o build/tiqb2-stage3
#   gate A:  stage1/stage2/stage3 emit byte-identical C for tiq_main.tiq, so
#            the fixed point holds under Tiq-to-Tiq staging, not just under
#            host-cc staging
#   gate B:  stage3 compiles and runs a hello fixture
#   gate C:  stage3 compiles examples/grep.tiq and that binary searches a file
#   gate D:  stage3 fails closed on an unsupported command (exit 2, no output
#            artifact), on a source it cannot check (exit 1), and on a source
#            path that does not exist (exit 1, located E27, no artifact)
#   gate E:  no *.tiqtmp.c scratch file survives anywhere in build/ or the
#            temp directory
#
# What this proves: a Tiq-built binary compiles a Tiq program end to end,
# deterministically, three stages deep, after a single seeding by the C
# bootstrap. What it does NOT prove: the host C compiler is still the backend
# and is still required, so the M17 exit gate
# (docs/POST_BOOTSTRAP_ROADMAP.md, "self-hosting without a C compiler") is not
# met by this harness.
#
# Fail-closed like tests/bootstrap.sh: missing prerequisites abort, every stage
# must succeed, previous artifacts are removed up front, and no gate can pass
# without the full sequence running in this invocation.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TIQ="$ROOT/build/tiq"
SRC="$ROOT/src/tiq/tiq_main.tiq"
GREP_SRC="$ROOT/examples/grep.tiq"
BUILD="$ROOT/build"
CC_BIN="${CC:-cc}"

fail=0
if [ ! -x "$TIQ" ]; then
    echo "bootstrap2: FAIL $TIQ not built; run 'make' first" >&2
    fail=1
fi
if [ ! -f "$SRC" ]; then
    echo "bootstrap2: FAIL $SRC not found; the Tiq driver is the subject of this test" >&2
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    exit 1
fi

mkdir -p "$BUILD"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tiq-bootstrap2-XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# Remove artifacts from a previous run so a pass is impossible unless every
# stage of THIS run produced its own output.
rm -f "$BUILD/tiqb2-stage1" "$BUILD/tiqb2-stage2" "$BUILD/tiqb2-stage3"
rm -f "$BUILD/tiqb2-stage1.c" "$BUILD/tiqb2-stage2.c" "$BUILD/tiqb2-stage3.c"

# --- seed: the C bootstrap builds stage1 --------------------------------------
echo "bootstrap2: seed — C bootstrap builds build/tiqb2-stage1 from src/tiq/tiq_main.tiq"
if ! "$TIQ" build "$SRC" -o "$BUILD/tiqb2-stage1" >"$TMP_DIR/seed.log" 2>&1; then
    echo "bootstrap2: FAIL seed build" >&2
    grep -E "error" "$TMP_DIR/seed.log" | head -10 >&2
    fail=1
elif [ ! -x "$BUILD/tiqb2-stage1" ]; then
    echo "bootstrap2: FAIL seed produced no executable build/tiqb2-stage1" >&2
    fail=1
fi

# --- stage 1: the Tiq-built driver builds the next driver ---------------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap2: stage 1 — tiqb2-stage1 build src/tiq/tiq_main.tiq -o build/tiqb2-stage2"
    if ! "$BUILD/tiqb2-stage1" build "$SRC" -o "$BUILD/tiqb2-stage2" >"$TMP_DIR/s1.log" 2>&1; then
        echo "bootstrap2: FAIL stage 1 (a Tiq-built driver could not build its own source)" >&2
        sed -n '1,10p' "$TMP_DIR/s1.log" >&2
        fail=1
    elif [ ! -x "$BUILD/tiqb2-stage2" ]; then
        echo "bootstrap2: FAIL stage 1 produced no executable build/tiqb2-stage2" >&2
        fail=1
    fi
fi

# --- stage 2: the chain continues without the C bootstrap ---------------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap2: stage 2 — tiqb2-stage2 build src/tiq/tiq_main.tiq -o build/tiqb2-stage3"
    if ! "$BUILD/tiqb2-stage2" build "$SRC" -o "$BUILD/tiqb2-stage3" >"$TMP_DIR/s2.log" 2>&1; then
        echo "bootstrap2: FAIL stage 2" >&2
        sed -n '1,10p' "$TMP_DIR/s2.log" >&2
        fail=1
    elif [ ! -x "$BUILD/tiqb2-stage3" ]; then
        echo "bootstrap2: FAIL stage 2 produced no executable build/tiqb2-stage3" >&2
        fail=1
    fi
fi

# --- gate A: emitted C is identical across Tiq-built stages ------------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap2: gate A — every stage emits byte-identical C for its own source"
    for n in 1 2 3; do
        if ! "$BUILD/tiqb2-stage$n" emit-c "$SRC" > "$BUILD/tiqb2-stage$n.c" 2>"$TMP_DIR/emit$n.err"; then
            echo "bootstrap2: FAIL tiqb2-stage$n could not emit C" >&2
            sed -n '1,10p' "$TMP_DIR/emit$n.err" >&2
            fail=1
        elif [ ! -s "$BUILD/tiqb2-stage$n.c" ]; then
            echo "bootstrap2: FAIL tiqb2-stage$n produced empty C" >&2
            fail=1
        fi
    done
    if [ "$fail" -eq 0 ]; then
        s1=$(wc -c < "$BUILD/tiqb2-stage1.c" | tr -d ' ')
        if cmp -s "$BUILD/tiqb2-stage1.c" "$BUILD/tiqb2-stage2.c" && cmp -s "$BUILD/tiqb2-stage2.c" "$BUILD/tiqb2-stage3.c"; then
            echo "bootstrap2: gate A PASS — ${s1}B identical from stage1 through stage3"
        else
            echo "bootstrap2: FAIL gate A — the Tiq-to-Tiq stages disagree on emitted C" >&2
            diff "$BUILD/tiqb2-stage1.c" "$BUILD/tiqb2-stage2.c" 2>/dev/null | grep -m1 -E '^[0-9]' >&2 || true
            diff "$BUILD/tiqb2-stage2.c" "$BUILD/tiqb2-stage3.c" 2>/dev/null | grep -m1 -E '^[0-9]' >&2 || true
            fail=1
        fi
    fi
fi

# --- gate B: stage3 compiles and runs a hello fixture ------------------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap2: gate B — tiqb2-stage3 builds and runs a hello fixture"
    printf 'print("ok")\n' > "$TMP_DIR/hello.tiq"
    if ! "$BUILD/tiqb2-stage3" build "$TMP_DIR/hello.tiq" -o "$TMP_DIR/hello" 2>"$TMP_DIR/hello.err"; then
        echo "bootstrap2: FAIL gate B — stage3 could not build hello.tiq" >&2
        sed -n '1,10p' "$TMP_DIR/hello.err" >&2
        fail=1
    elif [ "$("$TMP_DIR/hello")" != "ok" ]; then
        echo "bootstrap2: FAIL gate B — the built binary printed '$("$TMP_DIR/hello")', expected 'ok'" >&2
        fail=1
    else
        echo "bootstrap2: gate B PASS — Tiq-built driver → compiled hello.tiq → 'ok'"
    fi
fi

# --- gate C: stage3 compiles a real program from examples/ ------------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap2: gate C — tiqb2-stage3 builds examples/grep.tiq"
    printf 'alpha\nbeta needle here\ngamma\nneedle again\n' > "$TMP_DIR/needles.txt"
    if ! "$BUILD/tiqb2-stage3" build "$GREP_SRC" -o "$TMP_DIR/grep" 2>"$TMP_DIR/grep.err"; then
        echo "bootstrap2: FAIL gate C — stage3 could not build grep.tiq" >&2
        sed -n '1,10p' "$TMP_DIR/grep.err" >&2
        fail=1
    else
        # Byte offsets are of the literal 'needle' occurrences in the fixture
        # above: line 2 starts at 7, line 4 at 30.
        "$TMP_DIR/grep" needle "$TMP_DIR/needles.txt" > "$TMP_DIR/grep.out"
        printf '7:\nbeta needle here\n30:\nneedle again\n' > "$TMP_DIR/grep.expected"
        if ! cmp -s "$TMP_DIR/grep.out" "$TMP_DIR/grep.expected"; then
            echo "bootstrap2: FAIL gate C — built grep output mismatch" >&2
            diff "$TMP_DIR/grep.expected" "$TMP_DIR/grep.out" >&2 || true
            fail=1
        elif "$TMP_DIR/grep" absent "$TMP_DIR/needles.txt" >/dev/null 2>&1; then
            echo "bootstrap2: FAIL gate C — grep exited 0 on a non-matching pattern" >&2
            fail=1
        else
            echo "bootstrap2: gate C PASS — built grep matches, and exits non-zero on no match"
        fi
    fi
fi

# --- gate D: the driver surface fails closed --------------------------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap2: gate D — unsupported commands and unchecked sources fail closed"
    "$BUILD/tiqb2-stage3" run "$TMP_DIR/hello.tiq" >"$TMP_DIR/reject.out" 2>&1
    if [ $? -ne 2 ]; then
        echo "bootstrap2: FAIL gate D — 'run' exited $?, expected 2" >&2
        fail=1
    fi
    "$BUILD/tiqb2-stage3" build "$TMP_DIR/hello.tiq" >"$TMP_DIR/reject2.out" 2>&1
    if [ $? -ne 2 ]; then
        echo "bootstrap2: FAIL gate D — 'build' without -o exited $?, expected 2" >&2
        fail=1
    fi
    printf 'x = 1 + "s"\n' > "$TMP_DIR/bad.tiq"
    "$BUILD/tiqb2-stage3" build "$TMP_DIR/bad.tiq" -o "$TMP_DIR/bad" >"$TMP_DIR/bad.out" 2>&1
    rc=$?
    if [ "$rc" -ne 1 ]; then
        echo "bootstrap2: FAIL gate D — a type error exited $rc, expected 1" >&2
        fail=1
    fi
    if ! grep -q "error\[E09\]" "$TMP_DIR/bad.out"; then
        echo "bootstrap2: FAIL gate D — the E09 diagnostic is missing from the driver output" >&2
        sed -n '1,5p' "$TMP_DIR/bad.out" >&2
        fail=1
    fi
    if [ -e "$TMP_DIR/bad" ]; then
        echo "bootstrap2: FAIL gate D — a failed check still produced an executable" >&2
        fail=1
    fi
    # The driver's most common user error is a mistyped source path. The C
    # bootstrap used to abort with a token-vector index panic here; the
    # flattener must stop at the located E27 instead.
    "$BUILD/tiqb2-stage3" build "$TMP_DIR/never-created.tiq" -o "$TMP_DIR/ghost" >"$TMP_DIR/ghost.out" 2>&1
    rc=$?
    if [ "$rc" -ne 1 ]; then
        echo "bootstrap2: FAIL gate D — a missing source exited $rc, expected 1" >&2
        sed -n '1,5p' "$TMP_DIR/ghost.out" >&2
        fail=1
    fi
    if ! grep -q "never-created.tiq:1: error\[E27\]: module not found:" "$TMP_DIR/ghost.out"; then
        echo "bootstrap2: FAIL gate D — no located E27 for a missing source" >&2
        sed -n '1,5p' "$TMP_DIR/ghost.out" >&2
        fail=1
    fi
    if grep -q 'out of bounds' "$TMP_DIR/ghost.out"; then
        echo "bootstrap2: FAIL gate D — the flattener panicked instead of failing closed" >&2
        fail=1
    fi
    if [ -e "$TMP_DIR/ghost" ] || [ -e "$TMP_DIR/ghost.tiqtmp.c" ]; then
        echo "bootstrap2: FAIL gate D — a missing source still produced an artifact" >&2
        fail=1
    fi
    ver=$("$BUILD/tiqb2-stage3" --version 2>/dev/null)
    vrc=$?
    if [ "$vrc" -ne 0 ] || [ "$ver" != "tiq-selfhost 0.1.0-dev - self-hosted Tiq compiler driver" ]; then
        echo "bootstrap2: FAIL gate D — '--version' exited $vrc and printed '$ver'" >&2
        fail=1
    fi
    [ "$fail" -eq 0 ] && echo "bootstrap2: gate D PASS — exit 2 on unsupported input, exit 1 plus E09 on a bad program and E27 on a missing source, --version on stdout"
fi

# --- gate E: no scratch C survives ------------------------------------------
if [ "$fail" -eq 0 ]; then
    left=$(find "$BUILD" "$TMP_DIR" -maxdepth 1 -name '*.tiqtmp.c' 2>/dev/null)
    if [ -n "$left" ]; then
        echo "bootstrap2: FAIL gate E — scratch C files survived:" >&2
        echo "$left" >&2
        fail=1
    else
        echo "bootstrap2: gate E PASS — no *.tiqtmp.c artifacts left behind"
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "bootstrap2: failed" >&2
    exit 1
fi
echo "bootstrap2: ok — one C-bootstrap seed, three Tiq-built stages, fixed point under Tiq-to-Tiq staging"
