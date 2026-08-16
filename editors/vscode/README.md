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
