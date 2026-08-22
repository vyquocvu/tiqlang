# Tiq Interactive Web Playground (M20.3)

An interactive, zero-installation browser playground for the [Tiq programming language](https://github.com/tiqlang/tiq).

## Features

- **Interactive Code Editor**: Write and edit Tiq code directly in your browser.
- **Client-Side WebAssembly Execution**: Runs compiled Tiq binaries in the browser using the WASI preview1 polyfill runtime.
- **Target Inspection**: Inspect emitted ISO C11 code, SSA intermediate representation (IR), or WASM output.
- **Preset Examples**: Pre-loaded examples covering language fundamentals:
  - Hello World
  - Fibonacci (`fib.tiq`)
  - Sieve of Eratosthenes (`primes.tiq`)
  - Option and Result types (`option_result.tiq`)
  - Pattern Matching (`pattern_matching.tiq`)
- **Shareable Links**: Share code snippets easily via base64 URL hashes.

## Running Locally

To run or preview the playground locally:

```sh
# Using Python
python3 -m http.server 8080 --directory editors/playground

# Or using Node / any static server
npx serve editors/playground
```

Open `http://localhost:8080` in your web browser.
