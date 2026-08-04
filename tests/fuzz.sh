#!/bin/sh
# Deterministic, seed-driven fuzz of the compiler front end via `tiq check`
# (plan item 0.4, extended by M21.2). Mutations of the checked-in corpus
# (examples/ plus the self-hosted compiler sources src/tiq/) using a
# fixed-seed LCG, so every run tests the identical inputs.
#
# Mutation operators (selected by the LCG, one per mutant):
#   0 byte replacement  — overwrite one byte with printable ASCII
#   1 byte deletion     — remove one byte
#   2 byte insertion    — insert one printable ASCII byte
#   3 truncation        — cut the input to a shorter prefix
#
# Properties asserted (fail-closed):
#   1. `tiq check` never crashes (no signal exits) on mutated input.
#   2. `tiq check` never emits an executable or any other artifact.
#
# CI runs this harness against the ASan/UBSan build (M21.2 continuous
# sanitizer fuzzing); locally, FUZZ_MUTATIONS=<n> deepens coverage while
# staying deterministic for each configured value.

set -eu

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-fuzz-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

# Fixed seed: determinism is non-negotiable.
seed=20260727
MUTATIONS_PER_FILE="${FUZZ_MUTATIONS:-8}"

next_rand() {
    # LCG (glibc constants), plain POSIX arithmetic — identical on every host.
    seed=$(( (seed * 1103515245 + 12345) % 2147483648 ))
}

fail=0
runs=0

mutate() {
    # mutate <src> <mutant>: apply one LCG-chosen operator.
    src="$1"
    mutant="$2"
    size=$(wc -c < "$src" | tr -d ' ')
    next_rand
    op=$(( seed % 4 ))
    next_rand
    pos=$(( seed % (size + 1) ))
    next_rand
    byte=$(( 33 + seed % 94 ))  # printable ASCII payload
    case "$op" in
    0)  # byte replacement
        cp "$src" "$mutant"
        printf "\\$(printf '%03o' "$byte")" | dd of="$mutant" bs=1 seek="$pos" conv=notrunc 2>/dev/null
        ;;
    *)
        # Shared prefix for delete/insert/truncate. BSD head rejects `-c 0`,
        # so zero-length prefixes are created directly.
        if [ "$pos" -gt 0 ]; then head -c "$pos" "$src" > "$mutant"; else : > "$mutant"; fi
        case "$op" in
        1)  # byte deletion (may be a no-op at EOF)
            tail -c +$((pos + 2)) "$src" >> "$mutant"
            ;;
        2)  # byte insertion
            printf "\\$(printf '%03o' "$byte")" >> "$mutant"
            tail -c +$((pos + 1)) "$src" >> "$mutant"
            ;;
        *)  # truncation (may produce an empty file)
            ;;
        esac
        ;;
    esac
}

# Sorted corpus walk keeps the mutation sequence stable. The corpus spans the
# examples and the self-hosted compiler sources, so mutants exercise small
# programs and large multi-import modules alike.
corpus=$( { find examples -name '*.tiq'; find src/tiq -maxdepth 1 -name '*.tiq'; } | sort )

for src in $corpus; do
    size=$(wc -c < "$src" | tr -d ' ')
    [ "$size" -gt 0 ] || continue

    i=0
    while [ $i -lt $MUTATIONS_PER_FILE ]; do
        i=$((i + 1))
        mutant="$TMP/mutant.tiq"
        mutate "$src" "$mutant"

        # Run the checker with a 10s watchdog; only exit codes 0/1 are
        # acceptable (a hang shows up as SIGALRM, i.e. exit > 1).
        set +e
        perl -e 'alarm 10; exec @ARGV' "$TIQ" check "$mutant" >/dev/null 2>&1
        code=$?
        set -e
        runs=$((runs + 1))

        if [ "$code" -gt 1 ]; then
            echo "FUZZ CRASH/HANG: $src op=$op pos=$pos exit=$code" >&2
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

echo "fuzz: ok ($runs mutated inputs, seed 20260727, operators replace/delete/insert/truncate)"
