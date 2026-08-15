#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd); tmp=${TMPDIR:-/tmp}/tiq-json-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM; mkdir -p "$tmp"
"$root/build/tiq" build "$root/src/tiq/tools/json.tiq" -o "$tmp/tiq-json"
printf ' { "a" : [true, null, -1.25e+2], "a": "x\\n" } \n' > "$tmp/good"
[ "$($tmp/tiq-json "$tmp/good")" = '{"a":[true,null,-1.25e+2],"a":"x\n"}' ]
for bad in '01' '"\q"' '[1,]' '{"a":1}x' '1.'; do printf %s "$bad" > "$tmp/bad"; ! "$tmp/tiq-json" "$tmp/bad" >"$tmp/o" 2>"$tmp/e"; grep -Eq '^invalid JSON at byte [0-9]+, line [0-9]+, column [0-9]+$' "$tmp/e"; done
printf 'json tool: ok\n'
