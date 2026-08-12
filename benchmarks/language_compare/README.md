# Tiq/C/Go/Rust/Python runtime comparison

This opt-in benchmark runs the same deterministic workloads in Tiq, C, Go,
Rust, and Python. Every language implements five cases:

- `arithmetic`: five million Park–Miller recurrence and integer-modulo steps;
- `array`: initialization plus five million data-dependent indexed reads; and
- `branches`: five million data-dependent three-way branch decisions;
- `function_calls`: one million inputs passed through recursive Euclidean GCD;
  and
- `matrix`: twenty multiplications of deterministic 64×64 integer matrices.

Every case has a pinned checksum in all five sources. The checksum gate
prevents failed or semantically different programs from being timed as
successful runs, and each case is built and timed separately.

From the repository root:

```sh
make
make benchmark-compare
# Faster exploratory run:
python3 benchmarks/language_compare/run.py --repeats 3 --warmup 1
```

Requirements are `python3`, `cc`, `go`, and `rustc`, plus `build/tiq`. Missing
tools fail closed rather than silently omitting a language. Override tool paths
with `PYTHON`, `CC`, `GO`, or `RUSTC`.

The table reports one build duration, median/minimum wall time after warmup,
and executable size. Python has no separate build or executable size. C and
Rust use optimization level 3; Go uses its standard release build; Tiq uses
`tiq build --release` (`-O3` through the C backend). Run on an otherwise idle machine, repeat the
measurement, and compare results only from the same host. This is a small CPU
microbenchmark suite, not a general language ranking: it does not represent
I/O, heap allocation, concurrency, application ergonomics, or compiler quality.
