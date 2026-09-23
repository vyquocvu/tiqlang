# Tiq Language Support for Vim

Syntax highlighting for [Tiq](https://github.com/vyquocvu/tiq) in Vim.

## Installation

### Manual

Copy the syntax and filetype detection files to your Vim configuration:

```sh
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp editors/vim/syntax/tiq.vim ~/.vim/syntax/
cp editors/vim/ftdetect/tiq.vim ~/.vim/ftdetect/
```

Restart Vim and open a `.tiq` file. Syntax highlighting should be active automatically.

### Vim Plug

Add to your `~/.vimrc`:

```vim
Plug 'tiqlang/tiq', { 'rtp': 'editors/vim' }
```

Then run `:PlugInstall`.

## Features

- **Keywords**: `struct`, `enum`, `match`, `skip`, `break`
- **Types**: `i64`, `i32`, `f64`, `bool`, `str`, `void`
- **Constants**: `true`, `false`, `none`, `some`, `ok`, `err`
- **Operators**: `->`, `??`, `<-`, `=`, arithmetic, comparison, logical, bitwise
- **Strings**: Double-quoted with escape sequences (`\n`, `\t`, `\\`, `\"`)
- **Comments**: Line comments with `//`
- **Numbers**: Decimal integers

## Verification

Open `editors/vscode/demo.tiq` (or any `.tiq` file) in Vim. Keywords, types, operators, strings, and comments should be colored distinctly.

## License

Same as the Tiq compiler.
