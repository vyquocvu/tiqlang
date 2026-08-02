#!/bin/sh
# M13.6: 3-stage bootstrap convergence verification.
#
# The M13.5 self-hosted C11 emitter produces valid, working C. M13.6 proves
# that the self-hosted compiler is a **convergent fixed point** under
# self-application: building it once with the C bootstrap, emitting its
# own C, rebuilding from that C, and emitting again must produce the same
# C bytes. This is the standard "bootstrap convergence" test for a
# self-hosted compiler and is the M13.6 exit gate per POST_BOOTSTRAP_ROADMAP.
#
# Sequence:
#   Stage 1: the C bootstrap (`./build/tiq`) builds `build/tiq-stage1`
#            from `src/tiq/emit_c_main.tiq`.
#   Stage 2: `tiq-stage1` emits C of its own source → `build/stage1.c`.
#   Stage 3: the host C compiler builds `build/tiq-stage2` from `stage1.c`
#            (using the same flags as the C bootstrap's `build` command,
#            i.e. `cc -std=c11 -Os -x c`), then `tiq-stage2` emits C of
#            the same source → `build/stage2.c`.
#   Gate:    `cmp -s build/stage1.c build/stage2.c` must succeed. If the
#            selfhost is a deterministic fixed point, stage1.c and
#            stage2.c are byte-identical regardless of which stage built
#            them. The verification (2026-08-02) confirmed both outputs
#            are 486,291 bytes.
#
# The M13.6 gate is the 3-stage convergence identity above — NOT byte
# identity with the C reference emitter. The selfhost embeds a different
# (Tiq-generated) runtime than the C reference's prelude chunks, so the two
# emitters are not byte-identical by design (486,291 vs 457,711 bytes for
# `src/tiq/emit_c_main.tiq`); cross-implementation formatting identity is not
# an M13.6 invariant. Functional equivalence with the reference is pinned by
# the 43-case differential harness `tests/selfhost_emit_c.sh`.
#
# This harness is fail-closed: missing prerequisites abort with a non-zero
# exit and a located diagnostic, every stage must succeed, every output must
# be freshly produced (stale artifacts are removed up front), and the gate
# cannot pass without running the full sequence.
#
# See `docs/POST_BOOTSTRAP_ROADMAP.md` M13.6 and `docs/M13_DETERMINISM.md`
# for the design rationale.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TIQ="$ROOT/build/tiq"
SRC="$ROOT/src/tiq/emit_c_main.tiq"
BUILD="$ROOT/build"
CC_BIN="${CC:-cc}"

# --- Fail-closed input checks -----------------------------------------------
fail=0
if [ ! -x "$TIQ" ]; then
    echo "bootstrap: FAIL $TIQ not built; run 'make' first" >&2
    fail=1
fi
if [ ! -f "$SRC" ]; then
    echo "bootstrap: FAIL $SRC not found; M13.6 requires the self-host source" >&2
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    exit 1
fi

mkdir -p "$BUILD"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tiq-bootstrap-XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# Remove any artifacts from a previous run so a pass is impossible without
# every stage of THIS run producing its output.
rm -f "$BUILD/tiq-stage1" "$BUILD/tiq-stage2" "$BUILD/stage1.c" "$BUILD/stage2.c"

# --- Stage 1: C bootstrap builds tiq-stage1 ---------------------------------
echo "bootstrap: stage 1 — C bootstrap builds build/tiq-stage1 from src/tiq/emit_c_main.tiq"
if [ "$fail" -eq 0 ]; then
    if ! "$TIQ" build "$SRC" -o "$BUILD/tiq-stage1" 2>"$TMP_DIR/stage1_build.err"; then
        echo "bootstrap: FAIL stage 1 build" >&2
        sed -n '1,10p' "$TMP_DIR/stage1_build.err" >&2 || true
        fail=1
    elif [ ! -x "$BUILD/tiq-stage1" ]; then
        echo "bootstrap: FAIL stage 1 produced no executable build/tiq-stage1" >&2
        fail=1
    fi
fi

# --- Stage 2: tiq-stage1 emits C of src/tiq/emit_c_main.tiq -----------------
if [ "$fail" -eq 0 ]; then
    echo "bootstrap: stage 2 — tiq-stage1 emits C of src/tiq/emit_c_main.tiq → build/stage1.c"
    if ! "$BUILD/tiq-stage1" "$SRC" > "$BUILD/stage1.c" 2>"$TMP_DIR/stage2_emit.err"; then
        echo "bootstrap: FAIL stage 2 emit" >&2
        sed -n '1,10p' "$TMP_DIR/stage2_emit.err" >&2 || true
        fail=1
    elif [ ! -s "$BUILD/stage1.c" ]; then
        echo "bootstrap: FAIL stage 2 produced empty build/stage1.c" >&2
        fail=1
    fi
fi

# --- Stage 3: host C compiler builds tiq-stage2 from stage1.c, then emits ---
if [ "$fail" -eq 0 ]; then
    echo "bootstrap: stage 3a — host cc builds build/tiq-stage2 from build/stage1.c"
    if ! "$CC_BIN" -std=c11 -Os -x c "$BUILD/stage1.c" -o "$BUILD/tiq-stage2" 2>"$TMP_DIR/stage3_compile.err"; then
        echo "bootstrap: FAIL stage 3a host C compile" >&2
        sed -n '1,10p' "$TMP_DIR/stage3_compile.err" >&2 || true
        fail=1
    elif [ ! -x "$BUILD/tiq-stage2" ]; then
        echo "bootstrap: FAIL stage 3a produced no executable build/tiq-stage2" >&2
        fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    echo "bootstrap: stage 3b — tiq-stage2 emits C of src/tiq/emit_c_main.tiq → build/stage2.c"
    if ! "$BUILD/tiq-stage2" "$SRC" > "$BUILD/stage2.c" 2>"$TMP_DIR/stage3_emit.err"; then
        echo "bootstrap: FAIL stage 3b emit" >&2
        sed -n '1,10p' "$TMP_DIR/stage3_emit.err" >&2 || true
        fail=1
    elif [ ! -s "$BUILD/stage2.c" ]; then
        echo "bootstrap: FAIL stage 3b produced empty build/stage2.c" >&2
        fail=1
    fi
fi

# --- Gate: stage1.c == stage2.c (convergence / fixed point) -----------------
if [ "$fail" -eq 0 ]; then
    s1_size=$(wc -c < "$BUILD/stage1.c" | tr -d ' ')
    s2_size=$(wc -c < "$BUILD/stage2.c" | tr -d ' ')
    if cmp -s "$BUILD/stage1.c" "$BUILD/stage2.c"; then
        echo "bootstrap: gate PASS — stage1.c (${s1_size}B) == stage2.c (${s2_size}B); selfhost is a convergent fixed point"
    else
        echo "bootstrap: FAIL gate — stage1.c (${s1_size}B) != stage2.c (${s2_size}B)" >&2
        # Show the first differing line to localize the regression.
        diff "$BUILD/stage1.c" "$BUILD/stage2.c" 2>/dev/null | grep -m1 -E '^[0-9]' >&2 || true
        fail=1
    fi
else
    echo "bootstrap: FAIL gate inputs missing (stage1.c=$([ -f "$BUILD/stage1.c" ] && echo yes || echo no), stage2.c=$([ -f "$BUILD/stage2.c" ] && echo yes || echo no))" >&2
fi

# --- Sanity: tiq-stage2 correctly compiles and runs a hello-world fixture ---
if [ "$fail" -eq 0 ]; then
    echo "bootstrap: sanity — tiq-stage2 compiles and runs a hello-world fixture"
    printf 'print("ok")\n' > "$TMP_DIR/hello.tiq"
    if ! "$BUILD/tiq-stage2" "$TMP_DIR/hello.tiq" > "$TMP_DIR/hello.c" 2>"$TMP_DIR/hello_emit.err"; then
        echo "bootstrap: FAIL tiq-stage2 did not emit C for hello.tiq" >&2
        sed -n '1,10p' "$TMP_DIR/hello_emit.err" >&2 || true
        fail=1
    elif ! "$CC_BIN" -std=c11 -Os -x c "$TMP_DIR/hello.c" -o "$TMP_DIR/hello_bin" 2>"$TMP_DIR/hello_compile.err"; then
        echo "bootstrap: FAIL hello.c did not compile" >&2
        sed -n '1,10p' "$TMP_DIR/hello_compile.err" >&2 || true
        fail=1
    else
        got=$("$TMP_DIR/hello_bin" 2>/dev/null)
        if [ "$got" = "ok" ]; then
            echo "bootstrap: sanity PASS — tiq-stage2 → compiled hello.tiq → 'ok'"
        else
            echo "bootstrap: FAIL hello binary printed '${got}', expected 'ok'" >&2
            fail=1
        fi
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "bootstrap: failed" >&2
    exit 1
fi
echo "bootstrap: ok"
