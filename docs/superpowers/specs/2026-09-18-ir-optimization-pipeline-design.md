# Tiq IR Optimization Pipeline Design

**Date:** 2026-09-18
**Status:** Approved for implementation

## Problem

The Tiq compiler has a functional SSA IR (`src/ir_lower.c`, `include/ir.h`) but zero optimization passes. Every program is lowered directly to backend code with no IR-level transformations. The compiler relies entirely on the host C compiler's optimizer for generated code performance. This leaves performance on the table, especially for the WebAssembly and native backends which have no host optimizer.

## Goal

Build a phased optimization pipeline on the existing SSA IR that:
1. Produces measurably faster and smaller executables
2. Benefits all backends (C, Wasm, native) uniformly
3. Maintains Tiq's fail-closed semantics — optimizations must never change observable behavior
4. Integrates cleanly into the existing compilation pipeline

## Architecture

### Pass Interface

```c
// include/opt.h
typedef struct {
    int changes;       // number of transformations applied
    bool modified;     // true if the IR was changed at all
} OptResult;

typedef OptResult (*OptPassFn)(IrModule *mod, IrFunction *func);

void opt_run_pipeline(IrModule *mod, const OptPassFn *passes, int pass_count);
```

Each pass receives the full module (for cross-function analysis) and a specific function. Passes return a change count so the pipeline can iterate to a fixed point.

### Integration Point

```
source -> lex -> parse -> semantic -> ir_lower -> [opt_pipeline] -> emit_c / emit_wasm / asm_*
```

New CLI flag `--opt-level=0|1|2`:
- `0` — no optimization (current behavior)
- `1` — Phase 1 passes (constant folding, DCE, algebraic simplification)
- `2` — Phase 1 + Phase 2 passes (CSE, copy propagation, codegen cleanup)

Default becomes `--opt-level=1`.

### File Layout

```
include/opt.h          — pass interface, OptResult, OptPassFn
src/opt/opt_pipeline.c — pass manager, fixed-point iteration
src/opt/opt_constfold.c — constant folding pass
src/opt/opt_dce.c      — dead code elimination pass
src/opt/opt_algebraic.c — algebraic simplification pass
src/opt/opt_cse.c      — common subexpression elimination (Phase 2)
src/opt/opt_copyprop.c  — copy propagation (Phase 2)
src/opt/opt_inline.c   — function inlining (Phase 3)
src/opt/opt_tco.c      — tail-call optimization (Phase 3)
src/opt/opt_licm.c     — loop-invariant code motion (Phase 3)
src/opt/opt_match.c    — match compilation (Phase 3)
src/opt/opt_util.c     — shared utilities (use-def, dominance)
```

### Utility Layer

Built incrementally as passes need them:
- **Phase 1:** Use-count array per register (`use_count[reg]`)
- **Phase 2:** Value numbering hash table for CSE
- **Phase 3:** Dominance frontier computation for inlining

## Phase 1: Quick Wins

### Pass 1: Constant Folding

Walk each function's instructions. When all operands of an arithmetic/comparison/logical/bitwise instruction are `IR_OP_IMM` constants, evaluate at compile time and replace with the appropriate `IR_CONST_*`.

**Rules by opcode:**
- `IR_ADD/SUB/MUL/DIV/MOD` on integer immediates -> compute, replace with `IR_CONST_INT`
- `IR_ADD/SUB/MUL/DIV` on float immediates (bit pattern in `imm`) -> compute, replace with `IR_CONST_FLOAT`
- `IR_CMP_EQ/NE/LT/LE/GT/GE` on two integer immediates -> `IR_CONST_BOOL`
- `IR_AND/OR/NOT` on bool immediates -> `IR_CONST_BOOL`
- `IR_BIT_AND/OR/XOR/SHL/SHR` on integer immediates -> `IR_CONST_INT`
- `IR_NEG` on integer/float immediate -> negate

**Safety:** Division by zero is skipped (left for runtime). Float special cases (NaN, inf) follow IEEE 754 via host `double` arithmetic.

**Conditional branch folding:** When `IR_CBR`'s condition operand is a register whose defining instruction is `IR_CONST_BOOL` (direct lookup: find the instruction with `dst == condition_reg`), replace the `IR_CBR` with `IR_BR` to the taken branch and mark the dead branch unreachable.

### Pass 2: Dead Code Elimination

**2a. Unused register elimination:** Build a use-count array (`use_count[reg]` = number of instructions that read `reg`). Any instruction with `dst != -1` whose `dst` has `use_count == 0` and no side effects (not `IR_CALL`, `IR_PRINT`, `IR_STORE`, `IR_RET`) is removed.

**2b. Unreachable block elimination:** Starting from the entry block, mark reachable blocks via branch targets. Any unmarked block's instructions are removed.

### Pass 3: Algebraic Simplification

Pattern-match single instructions and replace with simpler forms:

| Pattern | Replacement |
|---------|-------------|
| `ADD x, 0` / `ADD 0, x` | `COPY x` |
| `SUB x, 0` | `COPY x` |
| `MUL x, 1` / `MUL 1, x` | `COPY x` |
| `MUL x, 0` / `MUL 0, x` | `CONST_INT 0` |
| `DIV x, 1` | `COPY x` |
| `AND x, x` | `COPY x` |
| `OR x, x` | `COPY x` |
| `XOR x, x` | `CONST_INT 0` |
| `NOT(NOT(x))` | `COPY x` |

These run after constant folding so that newly-exposed constants get folded in the next iteration. The pipeline iterates all three passes until `changes == 0` (fixed point).

## Phase 2: Code Quality

### Pass 4: Common Subexpression Elimination (CSE)

For each basic block, maintain a hash table mapping `(opcode, operand_signature) -> register`. When an instruction's opcode + operands match a previous computation in the same block (and no intervening store/call could invalidate it), replace the instruction with `IR_COPY` of the earlier result.

**Invalidation:** Any `IR_STORE`, `IR_CALL`, or memory-writing instruction flushes the hash table (conservative — sufficient without full alias analysis).

### Pass 5: Copy Propagation

Walk instructions. When encountering `IR_COPY dst, src`, record `dst -> src` in a forwarding table. Subsequent uses of `dst` are rewritten to use `src` directly. The `IR_COPY` is then removed by DCE in the next iteration.

**Phi-awareness:** Copy propagation must not forward across phi node boundaries incorrectly. Phi sources are updated conservatively — only forward when the copy dominates the use.

### Pass 6: C Codegen Cleanup

Improve `emit_c.c` output quality (no IR changes, emitter-only):

- **Expression inlining:** When a register is defined by a single instruction and used exactly once, emit the expression inline rather than as a separate `tmp_N = ...` statement.
- **Remove redundant casts:** Track when a value's type already matches the context type; skip the cast.
- **Parenthesization cleanup:** Use operator precedence to emit minimal parentheses.
- **GNU extension removal:** Replace `__extension__({...})` statement expressions in match with a sequence of assignments to a result variable followed by an if/else chain — pure ISO C11.

## Phase 3: Advanced

### Pass 7: Function Inlining

For small functions (body <= N instructions, no recursion, no `IR_CALL` to external functions), substitute the callee's body at each call site:
1. Clone the callee's instructions into the caller
2. Remap registers to avoid collisions
3. Replace `IR_RET` with `IR_COPY` to the call's destination register
4. Remove the original `IR_CALL`

Inline threshold starts conservative (<= 10 instructions) and can be tuned.

### Pass 8: Tail-Call Optimization

When `IR_RET` immediately follows `IR_CALL` and the return types match, replace the call+ret pair with a tail-call marker. The C backend emits the call as a `return` statement; native backends emit `jmp` instead of `call; ret`.

### Pass 9: Loop-Invariant Code Motion (LICM)

For loop back-edges in the CFG, identify instructions in the loop body whose operands are all defined outside the loop. Hoist these to the loop preheader block.

### Pass 10: Match Compilation

Replace the linear if/else chain for match expressions with:
- **Small match (<= 4 arms):** Keep if/else (already efficient)
- **Medium match (5-16 arms, contiguous integer patterns):** Binary search tree of comparisons
- **Large match (>= 16 arms, dense integer patterns):** Jump table via computed `goto` (C backend) or indirect branch (native)

## Testing Strategy

### Unit Tests

Each pass gets a dedicated test file in `tests/test_opt_*.c`:
1. Construct an `IrModule` programmatically (hand-built IR)
2. Run the pass
3. Assert the expected instruction sequence via direct inspection or `ir_dump` comparison

### Integration Tests

`tests/test_opt_e2e.c` compiles `.tiq` source through the full pipeline and verifies output correctness — ensuring optimizations don't change semantics.

### Regression Tests

Existing test suite (`make test`) must continue to pass at every opt-level. New `.tiq` test programs exercise optimization-sensitive patterns (nested arithmetic, dead bindings, match expressions).

### Benchmarking

Update `docs/OPTIMIZATION_PLAN.md` baseline after each phase:
- Binary size (hello, fib, math_demo)
- Runtime performance (fib(40), matrix multiply, stream processing)
- Compile time overhead of optimization passes
- Peak compile RSS

## Constraints

- All code must compile with ISO C11 (`-std=c11 -Wall -Wextra -Wpedantic -Werror`)
- Optimizations must preserve Tiq's fail-closed semantics — unsupported or undefined inputs must still fail closed
- No speculative abstractions — each pass is concrete, tested, and measurable
- The C backend remains the canonical release path; optimization must not weaken it
- Follow TDD: failing test first, then implementation, then refactor
