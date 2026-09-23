# Tiq Language Support for VS Code

Official Visual Studio Code extension providing language support for [Tiq](https://github.com/vyquocvu/tiqlang), a tiny, deterministic compiled language for fast tools and services.

## Features

- **Syntax Highlighting**: Comprehensive TextMate grammar covering keywords, types, functions, control operators (`<-`, `->`, `??`, `?`), numeric literals, and escape sequences.
- **Language Configuration**: Auto-closing delimiters (`{}`, `[]`, `()`, `""`), bracket matching, comment toggling (`//`), and smart indentation.
- **Code Snippets**: Quick scaffolds for functions, structs, enums, pattern matching, loops, and modules.
- **LSP Ready**: Compatible with `tiq-lsp` (Language Server Protocol) for hover, go-to-definition, and semantic auto-completion.

## Installation

To install locally during development:

```sh
mkdir -p ~/.vscode/extensions/tiqlang.tiq-vscode-0.1.0
cp -r * ~/.vscode/extensions/tiqlang.tiq-vscode-0.1.0/
```

Then restart VS Code. Open a `.tiq` file and verify syntax highlighting is active.

## Manual Verification

The extension structure and JSON files are validated by `tests/vscode_extension.sh`, but **visual verification in a real VS Code is required** to confirm:

1. **Syntax Highlighting**: Open `editors/vscode/demo.tiq` — a comprehensive showcase covering keywords (`fn`, `struct`, `enum`, `match`), types (`i64`, `str`, `bool`), operators (`<-`, `->`, `??`, `?`, `&`, `|`, `^`, `<<`, `>>`), string escapes, and comments. Keywords, types, strings, and operators should be colored distinctly. Additional examples in `examples/` (e.g., `hello.tiq`, `fib.tiq`, `pattern_matching.tiq`) provide real-world usage patterns.

2. **LSP Connection**: Ensure `tiq` is built (`make`), then configure the LSP client in VS Code settings:
   ```json
   {
     "tiq.lsp.path": "/path/to/tiq/build/tiq",
     "tiq.lsp.args": ["lsp"]
   }
   ```
   Hover over a variable to see type information. Trigger completion with `Ctrl+Space`.

3. **Snippets**: Type `fn` and press `Tab` — a function scaffold should expand.

**Evidence**: Screenshots of highlighting + hover + completion in a live editor should be added to this README once verified. Until then, the extension is structurally valid but not visually confirmed.
