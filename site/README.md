# Tiq Doc Site (Hylo-inspired)

Static, dependency-free documentation site mirroring key sections of
https://hylo-lang.org/ — hero + live code sample, features, syntax table,
working examples, benchmarks, docs index, community — rewritten with Tiq content.

## Layout

- `index.html` — hero (tagline, CTAs, fib code card with Copy/Run), Why Tiq?,
  syntax at a glance, working examples, benchmarks, community. Mirrors Hylo's
  homepage structure.
- `docs.html` — quick start, CLI reference, quick reference, doc index
  (links to `docs/*.md`), status table.
- `examples.html` — copy-paste runnable programs (sources in `examples/`).
- `assets/styles.css`, `assets/site.js` — no frameworks, no CDN, no tracking.
  Search overlay opens with Ctrl+K like Hylo.

## Preview

```sh
python3 -m http.server -d site 8080
# open http://localhost:8080/index.html
```

Or from repo root: `python3 -m http.server --directory site 8080`.

Playground links point to `editors/playground/index.html` (static demo,
Run disabled until the WASM bridge lands — the site never claims otherwise).

## Deploy

Any static host works (GitHub Pages, Netlify, nginx). Example:

```sh
# GitHub Pages: publish site/ as the root, or copy it to docs/
cp -r site /tmp/tiq-site && cd /tmp/tiq-site && python3 -m http.server 8080
```

## Checks

```sh
sh tests/site.sh
```

Asserts: required files exist, no external CDN/framework imports, hero and
nav structure present, every `site/examples.html` code sample passes
`tiq check`, playground honesty preserved.
