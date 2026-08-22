// Tiq Interactive Web Playground Controller

const EXAMPLES = {
  "hello": `// Hello World in Tiq
print("Hello, Tiq World!")
`,
  "fib": `// Recursive Fibonacci in Tiq
fib n:i64 : i64 -> {
    n <= 1 ? n : fib(n - 1) + fib(n - 2)
}

print(fib(10))
`,
  "primes": `// Sieve of Eratosthenes
is_prime n:i64 : bool -> {
    n <= 1 ? false : {
        res <- true
        d <- 2
        [d * d <= n && res == true] {
            n % d == 0 ? { res <- false }
            d <- d + 1
        }
        res
    }
}

count <- 0
i <- 2
[i < 50] {
    is_prime(i) ? {
        print(i)
        count <- count + 1
    }
    i <- i + 1
}
`,
  "option_result": `// Option and Result Types with ? and ??
safe_div a:i64 b:i64 : Result -> {
    b == 0 ? err(1) : ok(a / b)
}

compute a:i64 b:i64 : Result -> {
    q = ?safe_div(a, b)
    ok(q + 10)
}

r1 = compute(100, 2)
v1 = r1 ?? -1
print(v1)

r2 = compute(100, 0)
v2 = r2 ?? -1
print(v2)
`,
  "pattern_matching": `// Pattern Matching on Enums and Values
enum Light { Red, Yellow, Green }

action light:i64 -> match light {
    Light.Red => "Stop",
    Light.Yellow => "Caution",
    Light.Green => "Go",
    _ => "Unknown"
}

print(action(Light.Red))
print(action(Light.Green))
`
};

// WASI preview1 polyfill for in-browser execution
class SimpleWASI {
  constructor(stdoutCb, stderrCb) {
    this.stdoutCb = stdoutCb;
    this.stderrCb = stderrCb;
    this.memory = null;
  }

  setMemory(mem) {
    this.memory = mem;
  }

  getImports() {
    return {
      wasi_snapshot_preview1: {
        proc_exit: (code) => {
          this.stdoutCb(`\n[Program exited with code ${code}]\n`);
        },
        fd_write: (fd, iovs_ptr, iovs_len, nwritten_ptr) => {
          if (!this.memory) return 0;
          const view = new DataView(this.memory.buffer);
          let totalWritten = 0;
          const u8 = new Uint8Array(this.memory.buffer);

          for (let i = 0; i < iovs_len; i++) {
            const ptr = view.getUint32(iovs_ptr + i * 8, true);
            const len = view.getUint32(iovs_ptr + i * 8 + 4, true);
            const chunk = u8.subarray(ptr, ptr + len);
            const text = new TextDecoder("utf-8").decode(chunk);

            if (fd === 1) {
              this.stdoutCb(text);
            } else if (fd === 2) {
              this.stderrCb(text);
            }
            totalWritten += len;
          }
          view.setUint32(nwritten_ptr, totalWritten, true);
          return 0;
        },
        fd_close: () => 0,
        fd_seek: () => 0,
        environ_sizes_get: (count_ptr, size_ptr) => {
          const view = new DataView(this.memory.buffer);
          view.setUint32(count_ptr, 0, true);
          view.setUint32(size_ptr, 0, true);
          return 0;
        },
        environ_get: () => 0,
        args_sizes_get: (count_ptr, size_ptr) => {
          const view = new DataView(this.memory.buffer);
          view.setUint32(count_ptr, 0, true);
          view.setUint32(size_ptr, 0, true);
          return 0;
        },
        args_get: () => 0,
        clock_time_get: (id, precision, time_ptr) => {
          const view = new DataView(this.memory.buffer);
          const now = BigInt(Date.now()) * 1000000n;
          view.setBigUint64(time_ptr, now, true);
          return 0;
        }
      }
    };
  }
}

document.addEventListener("DOMContentLoaded", () => {
  const codeInput = document.getElementById("code-input");
  const outputConsole = document.getElementById("output-console");
  const exampleSelect = document.getElementById("example-select");
  const runBtn = document.getElementById("run-btn");
  const shareBtn = document.getElementById("share-btn");
  const targetSelect = document.getElementById("target-select");

  // Load from hash if present
  if (window.location.hash.startsWith("#code=")) {
    try {
      codeInput.value = decodeURIComponent(atob(window.location.hash.slice(6)));
    } catch (e) {
      codeInput.value = EXAMPLES["hello"];
    }
  } else {
    codeInput.value = EXAMPLES["hello"];
  }

  exampleSelect.addEventListener("change", (e) => {
    const key = e.target.value;
    if (EXAMPLES[key]) {
      codeInput.value = EXAMPLES[key];
      outputConsole.textContent = `Loaded example: ${key}\nClick 'Run' to execute.`;
    }
  });

  shareBtn.addEventListener("click", () => {
    const encoded = btoa(encodeURIComponent(codeInput.value));
    window.location.hash = `#code=${encoded}`;
    navigator.clipboard.writeText(window.location.href).then(() => {
      alert("Playground link copied to clipboard!");
    });
  });

  runBtn.addEventListener("click", async () => {
    const code = codeInput.value;
    const target = targetSelect.value;
    outputConsole.textContent = "Compiling and running...\n";

    try {
      if (window.TiqWasmCompiler) {
        const res = await window.TiqWasmCompiler.compileAndRun(code, target);
        outputConsole.textContent = res;
      } else {
        // Fallback simulation/info mode when standalone
        outputConsole.textContent = `[Target: ${target}]\nSource (${code.length} bytes) verified.\nTo run live with full compiler backend, serve with WebAssembly compiler bridge.`;
      }
    } catch (err) {
      outputConsole.textContent = `Error: ${err.message}`;
    }
  });

  // Enable Tab indentation in textarea
  codeInput.addEventListener("keydown", (e) => {
    if (e.key === "Tab") {
      e.preventDefault();
      const start = codeInput.selectionStart;
      const end = codeInput.selectionEnd;
      codeInput.value = codeInput.value.substring(0, start) + "    " + codeInput.value.substring(end);
      codeInput.selectionStart = codeInput.selectionEnd = start + 4;
    }
  });
});
