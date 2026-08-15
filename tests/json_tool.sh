#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/tiq-json-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

"$root/build/tiq" build "$root/src/tiq/tools/json.tiq" -o "$tmp/tiq-json"

# Round-trip the checked-in corpus and compare semantic values with Python's
# independent standard-library implementation.
for input in "$root"/tests/fixtures/json/*.json; do
    "$tmp/tiq-json" "$input" > "$tmp/generated.json"
    python3 - "$input" "$tmp/generated.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    expected = json.load(source)
with open(sys.argv[2], encoding="utf-8") as generated:
    actual = json.load(generated)
if actual != expected:
    raise SystemExit("semantic round-trip mismatch")
PY
done

printf '%s' '{"a":[1,{"b":true}],"name":"old"}' > "$tmp/object.json"
[ "$("$tmp/tiq-json" "$tmp/object.json" --get a)" = '[1,{"b":true}]' ]
[ "$("$tmp/tiq-json" "$tmp/object.json" --set-string name 'new value')" = '{"a":[1,{"b":true}],"name":"new value"}' ]
[ "$("$tmp/tiq-json" "$tmp/object.json" --set-string added 'quote"slash\')" = '{"a":[1,{"b":true}],"name":"old","added":"quote\"slash\\"}' ]

assert_invalid() {
    name=$1
    value=$2
    printf '%s' "$value" > "$tmp/bad.json"
    if "$tmp/tiq-json" "$tmp/bad.json" >"$tmp/out" 2>"$tmp/err"; then
        echo "json tool: accepted invalid fixture $name" >&2
        exit 1
    fi
    grep -Eq '^invalid JSON at byte [0-9]+, line [0-9]+, column [0-9]+$' "$tmp/err"
}

assert_invalid leading-zero '01'
assert_invalid invalid-escape '"\q"'
assert_invalid trailing-comma '[1,]'
assert_invalid trailing-data '{"a":1}x'
assert_invalid truncated-number '1.'
assert_invalid empty-document ''

# Invalid UTF-8: an isolated continuation byte.
printf '\200' > "$tmp/bad.json"
if "$tmp/tiq-json" "$tmp/bad.json" >"$tmp/out" 2>"$tmp/err"; then
    echo "json tool: accepted invalid UTF-8" >&2
    exit 1
fi
grep -Eq '^invalid JSON at byte 0, line 1, column 1$' "$tmp/err"

# Exactly 128 nested arrays are accepted; the next level is rejected with a
# located diagnostic rather than exhausting the host stack.
python3 - "$tmp/deep-ok.json" "$tmp/deep-bad.json" <<'PY'
import sys
for path, depth in ((sys.argv[1], 128), (sys.argv[2], 129)):
    with open(path, "w", encoding="ascii") as output:
        output.write("[" * depth + "0" + "]" * depth)
PY
"$tmp/tiq-json" "$tmp/deep-ok.json" > /dev/null
if "$tmp/tiq-json" "$tmp/deep-bad.json" >"$tmp/out" 2>"$tmp/err"; then
    echo "json tool: accepted input beyond nesting limit" >&2
    exit 1
fi
grep -Eq '^invalid JSON at byte 128, line 1, column 129$' "$tmp/err"

printf 'json tool: ok\n'
