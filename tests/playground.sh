#!/bin/sh
# M20.3: Web Playground integrity tests
# The playground is currently a static demo — it cannot compile or run code.
# This test asserts that honest labeling is present so it cannot silently
# ship as a working playground.
set -eu

DIR="editors/playground"

# Check required files exist
test -f "$DIR/index.html" || { echo "playground: FAIL index.html missing" >&2; exit 1; }
test -f "$DIR/playground.js" || { echo "playground: FAIL playground.js missing" >&2; exit 1; }
test -f "$DIR/playground.css" || { echo "playground: FAIL playground.css missing" >&2; exit 1; }
test -f "$DIR/README.md" || { echo "playground: FAIL README.md missing" >&2; exit 1; }

# Check index.html references playground.css and playground.js
grep -q 'playground.css' "$DIR/index.html" || { echo "playground: FAIL css link missing" >&2; exit 1; }
grep -q 'playground.js' "$DIR/index.html" || { echo "playground: FAIL js link missing" >&2; exit 1; }

# Assert honest labeling: the playground must clearly state it's a static demo
grep -q 'Static Demo' "$DIR/index.html" || { echo "playground: FAIL missing 'Static Demo' label in index.html" >&2; exit 1; }
grep -q 'Static demo mode' "$DIR/playground.js" || { echo "playground: FAIL missing 'Static demo mode' in playground.js" >&2; exit 1; }
grep -q 'Static Demo' "$DIR/README.md" || { echo "playground: FAIL missing 'Static Demo' label in README.md" >&2; exit 1; }

# Assert the Run button is disabled (compiler bridge not implemented)
grep -q 'id="run-btn".*disabled' "$DIR/index.html" || { echo "playground: FAIL Run button must be disabled" >&2; exit 1; }

# Assert README does NOT claim client-side execution
! grep -q 'Client-Side WebAssembly Execution' "$DIR/README.md" || { echo "playground: FAIL README must not claim 'Client-Side WebAssembly Execution'" >&2; exit 1; }

# Check that example presets in playground.js can be syntax checked by tiq
TIQ=./build/tiq
TMP_DIR="${TMPDIR:-/tmp}/tiq-playground-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

# Extract and test examples
node -e '
const fs = require("fs");
const content = fs.readFileSync("'"$DIR"'/playground.js", "utf8");
const match = content.match(/const EXAMPLES = (\{[\s\S]*?\n\};)/);
if (!match) process.exit(1);
const fn = new Function("return " + match[1]);
const ex = fn();
for (const [k, v] of Object.entries(ex)) {
  fs.writeFileSync("'"$TMP_DIR"'/" + k + ".tiq", v);
}
'

for f in "$TMP_DIR"/*.tiq; do
  if ! $TIQ check "$f" >/dev/null 2>&1; then
    echo "playground: FAIL example check failed for $f" >&2
    exit 1
  fi
done

echo "playground: ok (static demo)"
