#!/bin/sh
# M20.2: VS Code extension integrity tests
# Validates the extension structure and JSON files are well-formed.
# Manual verification in a real VS Code is required for syntax highlighting,
# LSP connection, hover/completion/go-to-definition (see README.md).
set -eu

DIR="editors/vscode"

# Check required files exist
test -f "$DIR/package.json" || { echo "vscode: FAIL package.json missing" >&2; exit 1; }
test -f "$DIR/language-configuration.json" || { echo "vscode: FAIL language-configuration.json missing" >&2; exit 1; }
test -f "$DIR/syntaxes/tiq.tmLanguage.json" || { echo "vscode: FAIL tiq.tmLanguage.json missing" >&2; exit 1; }
test -f "$DIR/snippets/tiq.code-snippets" || { echo "vscode: FAIL tiq.code-snippets missing" >&2; exit 1; }
test -f "$DIR/README.md" || { echo "vscode: FAIL README.md missing" >&2; exit 1; }

# Validate JSON files are well-formed
node -e "JSON.parse(require('fs').readFileSync('$DIR/package.json'))" || { echo "vscode: FAIL package.json invalid JSON" >&2; exit 1; }
node -e "JSON.parse(require('fs').readFileSync('$DIR/language-configuration.json'))" || { echo "vscode: FAIL language-configuration.json invalid JSON" >&2; exit 1; }
node -e "JSON.parse(require('fs').readFileSync('$DIR/syntaxes/tiq.tmLanguage.json'))" || { echo "vscode: FAIL tiq.tmLanguage.json invalid JSON" >&2; exit 1; }
node -e "JSON.parse(require('fs').readFileSync('$DIR/snippets/tiq.code-snippets'))" || { echo "vscode: FAIL tiq.code-snippets invalid JSON" >&2; exit 1; }

# Assert package.json has required fields
node -e '
const pkg = JSON.parse(require("fs").readFileSync("'"$DIR"'/package.json"));
if (!pkg.name) { console.error("vscode: FAIL package.json missing name"); process.exit(1); }
if (!pkg.version) { console.error("vscode: FAIL package.json missing version"); process.exit(1); }
if (!pkg.engines || !pkg.engines.vscode) { console.error("vscode: FAIL package.json missing engines.vscode"); process.exit(1); }
if (!pkg.contributes || !pkg.contributes.languages) { console.error("vscode: FAIL package.json missing contributes.languages"); process.exit(1); }
if (!pkg.contributes.grammars) { console.error("vscode: FAIL package.json missing contributes.grammars"); process.exit(1); }
const lang = pkg.contributes.languages[0];
if (!lang.id || lang.id !== "tiq") { console.error("vscode: FAIL language id must be tiq"); process.exit(1); }
if (!lang.extensions || !lang.extensions.includes(".tiq")) { console.error("vscode: FAIL language must register .tiq extension"); process.exit(1); }
' || exit 1

# Assert tmLanguage.json has required fields
node -e '
const tml = JSON.parse(require("fs").readFileSync("'"$DIR"'/syntaxes/tiq.tmLanguage.json"));
if (!tml.scopeName || tml.scopeName !== "source.tiq") { console.error("vscode: FAIL tmLanguage scopeName must be source.tiq"); process.exit(1); }
if (!tml.patterns || !Array.isArray(tml.patterns)) { console.error("vscode: FAIL tmLanguage missing patterns array"); process.exit(1); }
' || exit 1

# Assert README documents manual verification steps
grep -q 'Manual Verification' "$DIR/README.md" || { echo "vscode: FAIL README must document manual verification steps" >&2; exit 1; }

echo "vscode: ok (structure validated, manual verification required)"
