#!/bin/sh
# Deterministic, seed-driven fuzz of the lexer/parser via `tiq check`
# (plan item 0.4). Byte mutations of the checked-in corpus (examples/)
# using a fixed-seed LCG, so every run tests the identical inputs.
#
# Properties asserted (fail-closed):
#   1. `tiq check` never crashes (no signal exits) on mutated input.
#   2. `tiq check` never emits an executable or any other artifact.

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-fuzz-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

# Fixed seed: determinism is non-negotiable.
seed=20260727
MUTATIONS_PER_FILE=8

next_rand() {
    # LCG (glibc constants), plain POSIX arithmetic — identical on every host.
    seed=$(( (seed * 1103515245 + 12345) % 2147483648 ))
}

fail=0
runs=0

# Sorted corpus walk keeps the mutation sequence stable.
for src in $(find examples -name '*.tiq' | sort); do
    size=$(wc -c < "$src" | tr -d ' ')
    [ "$size" -gt 0 ] || continue

    i=0
    while [ $i -lt $MUTATIONS_PER_FILE ]; do
        i=$((i + 1))
        next_rand
        pos=$(( seed % size ))
        next_rand
        byte=$(( 33 + seed % 94 ))  # printable ASCII mutation

        mutant="$TMP/mutant.tiq"
        cp "$src" "$mutant"
        printf "\\$(printf '%03o' "$byte")" | dd of="$mutant" bs=1 seek="$pos" conv=notrunc 2>/dev/null

        # Run the checker with a 10s watchdog; only exit codes 0/1 are
        # acceptable (a hang shows up as SIGALRM, i.e. exit > 1).
        set +e
        perl -e 'alarm 10; exec @ARGV' "$TIQ" check "$mutant" >/dev/null 2>&1
        code=$?
        set -e
        runs=$((runs + 1))

        if [ "$code" -gt 1 ]; then
            echo "FUZZ CRASH/HANG: $src pos=$pos byte=$byte exit=$code" >&2
            cp "$mutant" "$TMP/crash-$runs.tiq"
            fail=1
        fi

        # Fail-closed: check must never leave artifacts next to the input.
        leftover=$(find "$TMP" -type f ! -name '*.tiq' | head -1)
        if [ -n "$leftover" ]; then
            echo "FUZZ ARTIFACT: $leftover after checking mutant of $src" >&2
            fail=1
        fi
        rm -f "$mutant"
    done
done

if [ "$fail" -ne 0 ]; then
    echo "fuzz: FAILED (crash inputs kept in $TMP)" >&2
    # keep crash inputs visible before trap cleanup
    ls "$TMP" >&2 || true
    exit 1
fi

echo "fuzz: ok ($runs mutated inputs, seed 20260727)"
