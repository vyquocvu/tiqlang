#!/bin/sh
# Doc site integrity tests (Hylo-inspired static site in site/).
# Fails closed: missing files, CDN leakage, or stale code samples break the gate.
set -eu

SITE="site"

# 1. Required files exist
for f in "$SITE/index.html" "$SITE/docs.html" "$SITE/examples.html" \
         "$SITE/assets/styles.css" "$SITE/assets/site.js" "$SITE/README.md"; do
  test -f "$f" || { echo "site: FAIL $f missing" >&2; exit 1; }
done

# 2. Structure mirrors hylo-lang.org: nav, hero, search (Ctrl+K), examples, community
grep -q 'id="nav-links"' "$SITE/index.html" || { echo "site: FAIL nav missing" >&2; exit 1; }
grep -q 'class="hero' "$SITE/index.html" || { echo "site: FAIL hero missing" >&2; exit 1; }
grep -q 'search-overlay' "$SITE/index.html" || { echo "site: FAIL search overlay missing" >&2; exit 1; }
grep -q 'Syntax at a glance' "$SITE/index.html" || { echo "site: FAIL syntax section missing" >&2; exit 1; }
grep -q 'Working examples' "$SITE/index.html" || { echo "site: FAIL examples section missing" >&2; exit 1; }
grep -q 'Join the discussion' "$SITE/index.html" || { echo "site: FAIL community section missing" >&2; exit 1; }
grep -q 'hylo-lang.org' "$SITE/index.html" || { echo "site: FAIL attribution missing" >&2; exit 1; }

# 3. No external dependencies — fully static, no CDN/framework/tracking
if grep -rEi 'https?://(cdn|unpkg|cdnjs|googleapis|gstatic)' "$SITE" --include='*.html' --include='*.js' --include='*.css' >/dev/null 2>&1; then
  echo "site: FAIL external CDN import found" >&2; exit 1;
fi
if grep -rEi 'react|vue|angular|next\.js|google-analytics|gtag' "$SITE" --include='*.html' --include='*.js' >/dev/null 2>&1; then
  echo "site: FAIL framework/tracker import found" >&2; exit 1;
fi

# 4. Honesty: site must not claim the playground compiles/runs code
if grep -rEi 'click Run to compile|runs? in your browser|live compiler' "$SITE" --include='*.html' >/dev/null 2>&1; then
  echo "site: FAIL must not claim live playground execution" >&2; exit 1;
fi

# 5. Code samples in examples.html must pass `tiq check` (deterministic failing test gate)
TIQ=./build/tiq
if [ ! -x "$TIQ" ]; then
  echo "site: SKIP tiq binary missing (run make first)" >&2; exit 0
fi
TMP_DIR="${TMPDIR:-/tmp}/tiq-site-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

python3 - "$SITE/examples.html" "$TMP_DIR" <<'EOF'
import re, sys, html
src, outdir = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
blocks = re.findall(r"<pre id=\"(ex-[^\"]+)\">(.*?)</pre>", text, re.S)
assert blocks, "no example <pre> blocks found"
for i, (pid, code) in enumerate(blocks):
    code = re.sub(r"<[^>]+>", "", code)
    code = html.unescape(code)
    open(f"{outdir}/{pid}.tiq", "w", encoding="utf-8").write(code + "\n")
print(f"extracted {len(blocks)} samples")
EOF

n=0
for f in "$TMP_DIR"/ex-*.tiq; do
  n=$((n + 1))
  if ! "$TIQ" check "$f" >/dev/null 2>&1; then
    echo "site: FAIL tiq check failed for $f" >&2
    "$TIQ" check "$f" >&2 || true
    exit 1
  fi
done
test "$n" -ge 5 || { echo "site: FAIL expected >=5 samples, got $n" >&2; exit 1; }

echo "site: ok ($n samples checked)"
