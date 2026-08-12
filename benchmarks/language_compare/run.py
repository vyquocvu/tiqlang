#!/usr/bin/env python3
"""Build and run deterministic workloads across five languages."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CASES = {
    "arithmetic": (5_000_000, "497907426"),
    "array": (5_000_000, "461852210"),
    "branches": (5_000_000, "37816615"),
    "function_calls": (1_000_000, "7270106"),
    "matrix": (20, "362194569"),
}
LANGUAGES = ("tiq", "c", "go", "rust", "python")
EXTENSIONS = {"tiq": "tiq", "c": "c", "go": "go", "rust": "rs", "python": "py"}
MARKER = re.compile(r"BENCH_ITERATIONS=(\d+) BENCH_EXPECTED=(\d+)")


def source_path(case: str, language: str) -> Path:
    return HERE / f"{case}.{EXTENSIONS[language]}"


def contract_check() -> None:
    for case, (iterations, expected) in CASES.items():
        for language in LANGUAGES:
            source = source_path(case, language)
            if not source.is_file():
                raise RuntimeError(f"missing {case}/{language} source: {source}")
            match = MARKER.search(source.read_text(encoding="utf-8"))
            if not match:
                raise RuntimeError(f"missing benchmark marker: {source}")
            if (int(match.group(1)), match.group(2)) != (iterations, expected):
                raise RuntimeError(f"benchmark marker drift: {source}")


def tool(name: str, env_name: str | None = None) -> str | None:
    candidate = os.environ.get(env_name, name) if env_name else name
    return shutil.which(candidate)


def timed(command: list[str], *, cwd: Path = ROOT) -> tuple[float, str]:
    start = time.perf_counter_ns()
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stderr}"
        )
    return elapsed_ms, result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate sources only")
    parser.add_argument("--repeats", type=int, default=5, help="timed runs per language")
    parser.add_argument("--warmup", type=int, default=1, help="untimed runs per language")
    args = parser.parse_args()
    if args.repeats < 1 or args.warmup < 0:
        parser.error("--repeats must be positive and --warmup non-negative")

    try:
        contract_check()
        if args.check:
            print("benchmark check: ok (5 cases; tiq,c,go,rust,python)")
            return 0

        tiq = ROOT / "build" / "tiq"
        python = tool("python3", "PYTHON")
        cc = tool("cc", "CC")
        go = tool("go", "GO")
        rustc = tool("rustc", "RUSTC")
        with tempfile.TemporaryDirectory(prefix="tiq-language-bench-") as raw_tmp:
            tmp = Path(raw_tmp)
            print(f"runs: {args.repeats} measured after {args.warmup} warmup(s)")
            for case, (iterations, expected) in CASES.items():
                commands: dict[str, tuple[list[str] | None, list[str]]] = {}
                outputs = {name: tmp / f"{case}-{name}" for name in LANGUAGES}
                if tiq.is_file():
                    commands["tiq"] = ([str(tiq), "build", "--release", str(source_path(case, "tiq")), "-o", str(outputs["tiq"])], [str(outputs["tiq"])])
                if cc:
                    commands["c"] = ([cc, "-std=c11", "-O3", str(source_path(case, "c")), "-o", str(outputs["c"])], [str(outputs["c"])])
                if go:
                    commands["go"] = ([go, "build", "-o", str(outputs["go"]), str(source_path(case, "go"))], [str(outputs["go"])])
                if rustc:
                    commands["rust"] = ([rustc, "-C", "opt-level=3", str(source_path(case, "rust")), "-o", str(outputs["rust"])], [str(outputs["rust"])])
                if python:
                    commands["python"] = (None, [python, str(source_path(case, "python"))])
                missing = [name for name in LANGUAGES if name not in commands]
                if missing:
                    raise RuntimeError("missing required tools: " + ", ".join(missing))

                print(f"\ncase: {case} ({iterations} iterations; checksum {expected})")
                print("language  build_ms  median_ms  min_ms  binary_bytes")
                for language in LANGUAGES:
                    build, run = commands[language]
                    binary_size = "n/a"
                    build_ms = 0.0
                    if build:
                        build_ms, _ = timed(build)
                        binary_size = str(Path(run[0]).stat().st_size)
                    for _ in range(args.warmup):
                        _, output = timed(run)
                        if output != expected:
                            raise RuntimeError(f"{case}/{language}: expected {expected}, got {output!r}")
                    samples = []
                    for _ in range(args.repeats):
                        elapsed, output = timed(run)
                        if output != expected:
                            raise RuntimeError(f"{case}/{language}: expected {expected}, got {output!r}")
                        samples.append(elapsed)
                    build_text = "n/a" if build is None else f"{build_ms:.1f}"
                    print(f"{language:<8} {build_text:>8} {statistics.median(samples):>10.1f} {min(samples):>7.1f} {binary_size:>13}")
        return 0
    except RuntimeError as error:
        print(f"benchmark: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
