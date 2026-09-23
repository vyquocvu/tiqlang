# Tiq Interactive Web Playground (M20.3)

**Status: Static Demo — Not Yet Functional**

This playground shows Tiq syntax and preset examples but **cannot compile or run code**. The WebAssembly compiler bridge is not yet implemented.

## Current State

- **Interactive Code Editor**: Write and edit Tiq code directly in your browser.
- **Preset Examples**: Pre-loaded examples covering language fundamentals.
- **Shareable Links**: Share code snippets via base64 URL hashes.
- **Compilation/Execution**: **NOT IMPLEMENTED** — the Run button is disabled.

## Building the Compiler Bridge

To make this a live playground, implement one of these approaches:

### Option A: Client-Side WebAssembly (B1-a)

Build the Tiq compiler to WebAssembly via Emscripten, expose `window.TiqWasmCompiler.compileAndRun(source, target)` from JavaScript, and instantiate the emitted wasm module in-page.

**Challenges:**
- Emscripten toolchain in CI
- The C bootstrap uses POSIX APIs (`fork`/`execvp`, `clock_gettime`) that need stubbing
- ~1 MB+ download for the compiler wasm

### Option B: Server-Side Compile (B1-b)

A small HTTP service (ideally written in Tiq, dogfooding `std/http.tiq`) that takes source, runs `tiq build --target wasm32-wasi`, and returns the wasm module. The browser instantiates it.

**Advantages:**
- Reuses the existing M17.4.2 wasm backend directly
- Dogfoods `std/http.tiq` (serving two roadmap items at once)
- Avoids adding Emscripten to CI

**Dependencies:**
- Requires M17.4.5 (IR numeric conversions) to compile programs beyond basic arithmetic
- Requires hosting infrastructure

## Running Locally (Static Demo)

To preview the playground locally:

```sh
# Using Python
python3 -m http.server 8080 --directory editors/playground

# Or using Node / any static server
npx serve editors/playground
```

Open `http://localhost:8080` in your web browser.
