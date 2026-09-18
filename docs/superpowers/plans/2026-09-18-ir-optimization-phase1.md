# IR Optimization Pipeline — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the optimization pass infrastructure and Phase 1 passes (constant folding, dead code elimination, algebraic simplification) to the Tiq compiler's SSA IR, integrated via a `--opt-level` CLI flag.

**Architecture:** A new `src/opt/` directory houses optimization passes with a uniform `OptResult` interface. The pipeline slots between `ir_lower()` and backend emitters in `main.c`. Phase 1 iterates three passes to a fixed point: constant folding, DCE, algebraic simplification.

**Tech Stack:** ISO C11, existing SSA IR (`include/ir.h`, `src/ir.c`), custom unit test harness (`tests/unit/test_main.c`), Make-based build.

**Spec:** `docs/superpowers/specs/2026-09-18-ir-optimization-pipeline-design.md`

---

## File Structure

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `include/opt.h` | Pass interface: `OptResult`, `OptPassFn`, `opt_run_pipeline()` |
| Create | `src/opt/opt_pipeline.c` | Pass manager: fixed-point iteration over pass array |
| Create | `src/opt/opt_constfold.c` | Constant folding pass |
| Create | `src/opt/opt_dce.c` | Dead code elimination pass |
| Create | `src/opt/opt_algebraic.c` | Algebraic simplification pass |
| Modify | `Makefile:15-28,33-36` | Add `src/opt/*.c` to build with subdirectory pattern rule |
| Modify | `src/main.c:340-369,723-735,835-839,1015-1031,1188-1229` | Add `--opt-level` flag, call pipeline at integration points |
| Modify | `tests/unit/test_main.c` | Add unit tests for all three passes |
| Create | `tests/opt.sh` | Integration test: compile `.tiq` files with `--opt-level=1` and verify output |

---

### Task 1: Pass Interface Header

**Files:**
- Create: `include/opt.h`

- [ ] **Step 1: Create the opt.h header**

```c
// include/opt.h
#ifndef TIQ_OPT_H
#define TIQ_OPT_H

#include <stdbool.h>
#include "ir.h"

typedef struct {
    int changes;
    bool modified;
} OptResult;

typedef OptResult (*OptPassFn)(IrModule *mod, IrFunction *func);

OptResult opt_constfold(IrModule *mod, IrFunction *func);
OptResult opt_dce(IrModule *mod, IrFunction *func);
OptResult opt_algebraic(IrModule *mod, IrFunction *func);

void opt_run_pipeline(IrModule *mod, int opt_level);

#endif
```

- [ ] **Step 2: Verify it compiles standalone**

Run: `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I include include/opt.h`
Expected: no output (success)

- [ ] **Step 3: Commit**

```bash
git add include/opt.h
git commit -m "feat(opt): add optimization pass interface header"
```

---

### Task 2: Pass Manager (opt_pipeline.c)

**Files:**
- Create: `src/opt/opt_pipeline.c`

- [ ] **Step 1: Write the failing unit test for the pipeline runner**

Add to `tests/unit/test_main.c`, after the existing includes (line 22), add:

```c
#include "../../include/ir.h"
#include "../../include/opt.h"
```

Add before `main()` in `test_main.c`:

```c
static void test_opt_pipeline_level0_is_noop(void) {
    IrModule mod;
    ir_module_init(&mod);
    IrFunction func;
    ir_func_init(&func, "f", 1);
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    IrOperand imm = { .kind = IR_OP_IMM, .imm = 42 };
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm, 1, 0);
    ir_add_block(&func, 0);
    func.blocks[0].instr_start = 0;
    func.blocks[0].instr_end = func.instr_count;

    mod.funcs = malloc(sizeof(IrFunction));
    mod.funcs[0] = func;
    mod.func_count = 1;

    int before = func.instr_count;
    opt_run_pipeline(&mod, 0);
    ASSERT(mod.funcs[0].instr_count == before);

    ir_module_free(&mod);
}
```

Add call in `main()` before `printf`:

```c
test_opt_pipeline_level0_is_noop();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make clean && make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g' && make test-unit`
Expected: FAIL — linker error: `opt_run_pipeline` undefined

- [ ] **Step 3: Implement opt_pipeline.c**

```c
// src/opt/opt_pipeline.c
#include "../include/opt.h"

#define MAX_ITERATIONS 10

void opt_run_pipeline(IrModule *mod, int opt_level) {
    if (opt_level <= 0) return;

    const OptPassFn phase1[] = { opt_constfold, opt_dce, opt_algebraic };
    const int phase1_count = sizeof(phase1) / sizeof(phase1[0]);

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        int total_changes = 0;
        for (int f = 0; f < mod->func_count; f++) {
            for (int p = 0; p < phase1_count; p++) {
                OptResult r = phase1[p](mod, &mod->funcs[f]);
                total_changes += r.changes;
            }
        }
        if (total_changes == 0) break;
    }
}
```

- [ ] **Step 4: Add stub implementations for the three passes so it links**

Create `src/opt/opt_constfold.c`:

```c
// src/opt/opt_constfold.c
#include "../include/opt.h"

OptResult opt_constfold(IrModule *mod, IrFunction *func) {
    (void)mod; (void)func;
    OptResult r = { .changes = 0, .modified = false };
    return r;
}
```

Create `src/opt/opt_dce.c`:

```c
// src/opt/opt_dce.c
#include "../include/opt.h"

OptResult opt_dce(IrModule *mod, IrFunction *func) {
    (void)mod; (void)func;
    OptResult r = { .changes = 0, .modified = false };
    return r;
}
```

Create `src/opt/opt_algebraic.c`:

```c
// src/opt/opt_algebraic.c
#include "../include/opt.h"

OptResult opt_algebraic(IrModule *mod, IrFunction *func) {
    (void)mod; (void)func;
    OptResult r = { .changes = 0, .modified = false };
    return r;
}
```

- [ ] **Step 5: Update Makefile to build src/opt/*.c**

Add after line 20 in `Makefile` (after `SRCS` definition):

```makefile
OPT_SRCS = src/opt/opt_pipeline.c src/opt/opt_constfold.c src/opt/opt_dce.c src/opt/opt_algebraic.c
OPT_OBJS = $(OPT_SRCS:src/opt/%.c=$(BUILD)/opt_%.o)
```

Change line 21 (`OBJS` line) to include `$(OPT_OBJS)`:

```makefile
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o) $(OPT_OBJS)
```

Add after line 28 (after the existing pattern rule):

```makefile
$(BUILD)/opt_%.o: src/opt/%.c $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 6: Run test to verify it passes**

Run: `make clean && make && make test-unit`
Expected: PASS — `test_opt_pipeline_level0_is_noop` passes, all existing tests pass

- [ ] **Step 7: Commit**

```bash
git add src/opt/opt_pipeline.c src/opt/opt_constfold.c src/opt/opt_dce.c src/opt/opt_algebraic.c
git add Makefile tests/unit/test_main.c
git commit -m "feat(opt): add pass manager with fixed-point iteration"
```

---

### Task 3: Constant Folding Pass

**Files:**
- Create: `src/opt/opt_constfold.c` (replace stub)
- Modify: `tests/unit/test_main.c`

- [ ] **Step 1: Write failing tests for constant folding**

Add to `tests/unit/test_main.c` before `main()`:

```c
static IrFunction make_test_func(void) {
    IrFunction func;
    ir_func_init(&func, "test", 4);
    ir_add_block(&func, 0);
    return func;
}

static IrModule make_single_func_module(IrFunction func) {
    IrModule mod;
    ir_module_init(&mod);
    mod.funcs = malloc(sizeof(IrFunction));
    mod.funcs[0] = func;
    mod.func_count = 1;
    mod.func_cap = 1;
    return mod;
}

static void test_opt_constfold_integer_add(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm3 = { .kind = IR_OP_IMM, .imm = 3 };
    IrOperand imm4 = { .kind = IR_OP_IMM, .imm = 4 };
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm3, 1, 0);
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm4, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_ADD, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_constfold(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    // The ADD should be replaced with CONST_INT 7
    IrInstr *add_instr = &mod.funcs[0].instrs[2];
    ASSERT(add_instr->op == IR_CONST_INT);
    ASSERT(add_instr->operands[0].imm == 7);

    ir_module_free(&mod);
}

static void test_opt_constfold_integer_mul(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm5 = { .kind = IR_OP_IMM, .imm = 5 };
    IrOperand imm6 = { .kind = IR_OP_IMM, .imm = 6 };
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm5, 1, 0);
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm6, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_MUL, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_constfold(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    IrInstr *mul_instr = &mod.funcs[0].instrs[2];
    ASSERT(mul_instr->op == IR_CONST_INT);
    ASSERT(mul_instr->operands[0].imm == 30);

    ir_module_free(&mod);
}

static void test_opt_constfold_comparison(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    IrType boolty = { .kind = IR_BOOL, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm10 = { .kind = IR_OP_IMM, .imm = 10 };
    IrOperand imm20 = { .kind = IR_OP_IMM, .imm = 20 };
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm10, 1, 0);
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm20, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_CMP_LT, r2, boolty, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_constfold(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    IrInstr *cmp_instr = &mod.funcs[0].instrs[2];
    ASSERT(cmp_instr->op == IR_CONST_BOOL);
    ASSERT(cmp_instr->operands[0].imm == 1);  // 10 < 20 is true

    ir_module_free(&mod);
}

static void test_opt_constfold_div_by_zero_skipped(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm10 = { .kind = IR_OP_IMM, .imm = 10 };
    IrOperand imm0 = { .kind = IR_OP_IMM, .imm = 0 };
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm10, 1, 0);
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm0, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_DIV, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_constfold(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 0);  // div by zero not folded
    ASSERT(mod.funcs[0].instrs[2].op == IR_DIV);  // unchanged

    ir_module_free(&mod);
}

static void test_opt_constfold_non_constant_not_folded(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand reg_op = { .kind = IR_OP_REG, .reg = r0 };
    IrOperand imm5 = { .kind = IR_OP_IMM, .imm = 5 };
    ir_emit_instr(&func, IR_ADD, r2, i64, (IrOperand[]){ reg_op, imm5 }, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_constfold(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 0);
    ASSERT(mod.funcs[0].instrs[0].op == IR_ADD);

    ir_module_free(&mod);
}

static void test_opt_constfold_cbr_constant_condition(void) {
    IrFunction func = make_test_func();
    IrType boolty = { .kind = IR_BOOL, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int bb1 = ir_add_block(&func, 1);
    int bb2 = ir_add_block(&func, 2);
    // r0 = true
    IrOperand imm_true = { .kind = IR_OP_IMM, .imm = 1 };
    ir_emit_instr(&func, IR_CONST_BOOL, r0, boolty, &imm_true, 0);
    // CBR r0, bb1, bb2  (should fold to BR bb1)
    IrOperand cbr_ops[3] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_BLOCK, .block = bb1 },
        { .kind = IR_OP_BLOCK, .block = bb2 }
    };
    ir_emit_instr(&func, IR_CBR, -1, (IrType){ .kind = IR_VOID, .semantic = NULL }, cbr_ops, 3, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_constfold(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    IrInstr *cbr_instr = &mod.funcs[0].instrs[1];
    ASSERT(cbr_instr->op == IR_BR);
    ASSERT(cbr_instr->operands[0].kind == IR_OP_BLOCK);
    ASSERT(cbr_instr->operands[0].block == bb1);  // took the true branch

    ir_module_free(&mod);
}
```

Add calls in `main()`:

```c
test_opt_constfold_integer_add();
test_opt_constfold_integer_mul();
test_opt_constfold_comparison();
test_opt_constfold_div_by_zero_skipped();
test_opt_constfold_non_constant_not_folded();
test_opt_constfold_cbr_constant_condition();
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make clean && make && make test-unit`
Expected: FAIL — assertions fail because stub returns 0 changes

- [ ] **Step 3: Implement constant folding**

Replace `src/opt/opt_constfold.c`:

```c
// src/opt/opt_constfold.c
#include <stdlib.h>
#include <string.h>
#include "../include/opt.h"

static bool is_int_imm(IrFunction *func, IrOperand *op, long long *out) {
    if (op->kind == IR_OP_IMM) {
        *out = op->imm;
        return true;
    }
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_INT) {
                *out = func->instrs[i].operands[0].imm;
                return true;
            }
        }
    }
    return false;
}

static bool is_bool_imm(IrFunction *func, IrOperand *op, long long *out) {
    if (op->kind == IR_OP_IMM) {
        *out = op->imm;
        return true;
    }
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_BOOL) {
                *out = func->instrs[i].operands[0].imm;
                return true;
            }
        }
    }
    return false;
}

static bool is_float_imm(IrFunction *func, IrOperand *op, double *out) {
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_FLOAT) {
                long long bits = func->instrs[i].operands[0].imm;
                memcpy(out, &bits, sizeof(double));
                return true;
            }
        }
    }
    return false;
}

static void replace_with_int(IrInstr *instr, long long val) {
    instr->op = IR_CONST_INT;
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = val;
    instr->operand_count = 1;
}

static void replace_with_bool(IrInstr *instr, long long val) {
    instr->op = IR_CONST_BOOL;
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = val;
    instr->operand_count = 1;
}

static void replace_with_float(IrInstr *instr, double val) {
    instr->op = IR_CONST_FLOAT;
    long long bits;
    memcpy(&bits, &val, sizeof(double));
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = bits;
    instr->operand_count = 1;
}

OptResult opt_constfold(IrModule *mod, IrFunction *func) {
    (void)mod;
    OptResult result = { .changes = 0, .modified = false };

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        long long a, b;

        switch (instr->op) {
        case IR_ADD:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a + b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_SUB:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a - b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_MUL:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a * b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_DIV:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b) && b != 0) {
                replace_with_int(instr, a / b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_MOD:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b) && b != 0) {
                replace_with_int(instr, a % b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_NEG:
            if (instr->operand_count == 1 &&
                is_int_imm(func, &instr->operands[0], &a)) {
                replace_with_int(instr, -a);
                result.changes++;
                result.modified = true;
            } else {
                double fa;
                if (instr->operand_count == 1 &&
                    is_float_imm(func, &instr->operands[0], &fa)) {
                    replace_with_float(instr, -fa);
                    result.changes++;
                    result.modified = true;
                }
            }
            break;
        case IR_CMP_EQ:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a == b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_NE:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a != b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_LT:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a < b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_LE:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a <= b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_GT:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a > b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_CMP_GE:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, a >= b ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_AND:
            if (instr->operand_count == 2 &&
                is_bool_imm(func, &instr->operands[0], &a) &&
                is_bool_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, (a && b) ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_OR:
            if (instr->operand_count == 2 &&
                is_bool_imm(func, &instr->operands[0], &a) &&
                is_bool_imm(func, &instr->operands[1], &b)) {
                replace_with_bool(instr, (a || b) ? 1 : 0);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_NOT:
            if (instr->operand_count == 1 &&
                is_bool_imm(func, &instr->operands[0], &a)) {
                replace_with_bool(instr, a ? 0 : 1);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_AND:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a & b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_OR:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a | b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_XOR:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a ^ b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_SHL:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a << b);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_SHR:
            if (instr->operand_count == 2 &&
                is_int_imm(func, &instr->operands[0], &a) &&
                is_int_imm(func, &instr->operands[1], &b)) {
                replace_with_int(instr, a >> b);
                result.changes++;
                result.modified = true;
            }
            break;
        default:
            break;
        }

        // Float arithmetic folding (ADD/SUB/MUL/DIV on float consts)
        if ((instr->op == IR_ADD || instr->op == IR_SUB ||
             instr->op == IR_MUL || instr->op == IR_DIV) &&
            instr->dst_type.kind == IR_F64) {
            double fa, fb;
            if (instr->operand_count == 2 &&
                is_float_imm(func, &instr->operands[0], &fa) &&
                is_float_imm(func, &instr->operands[1], &fb)) {
                double rv = 0;
                switch (instr->op) {
                case IR_ADD: rv = fa + fb; break;
                case IR_SUB: rv = fa - fb; break;
                case IR_MUL: rv = fa * fb; break;
                case IR_DIV: rv = fa / fb; break;
                default: break;
                }
                replace_with_float(instr, rv);
                result.changes++;
                result.modified = true;
            }
        }

        // CBR folding: constant condition -> unconditional branch
        if (instr->op == IR_CBR && instr->operand_count == 3 &&
            instr->operands[0].kind == IR_OP_REG) {
            long long cond_val;
            if (is_bool_imm(func, &instr->operands[0], &cond_val)) {
                int target = cond_val ? instr->operands[1].block
                                      : instr->operands[2].block;
                instr->op = IR_BR;
                instr->operands[0].kind = IR_OP_BLOCK;
                instr->operands[0].block = target;
                instr->operand_count = 1;
                result.changes++;
                result.modified = true;
            }
        }
    }

    return result;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make clean && make && make test-unit`
Expected: PASS — all 5 constant folding tests pass

- [ ] **Step 5: Commit**

```bash
git add src/opt/opt_constfold.c tests/unit/test_main.c
git commit -m "feat(opt): constant folding pass for integer, float, bool, comparison ops"
```

---

### Task 4: Dead Code Elimination Pass

**Files:**
- Create: `src/opt/opt_dce.c` (replace stub)
- Modify: `tests/unit/test_main.c`

- [ ] **Step 1: Write failing tests for DCE**

Add to `tests/unit/test_main.c` before `main()`:

```c
static void test_opt_dce_removes_unused_register(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm3 = { .kind = IR_OP_IMM, .imm = 3 };
    IrOperand imm4 = { .kind = IR_OP_IMM, .imm = 4 };
    // r0 = 3 (used)
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm3, 1, 0);
    // r1 = 4 (unused — dead)
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm4, 1, 0);
    // r2 = r0 + r0 (uses r0, not r1)
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r0 }
    };
    ir_emit_instr(&func, IR_ADD, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_dce(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    // After DCE, the CONST_INT for r1 should be marked dead (op set to IR_VOID sentinel)
    // We check that instr_count decreased or the dead instr is neutralized
    int alive = 0;
    for (int i = 0; i < mod.funcs[0].instr_count; i++) {
        if (mod.funcs[0].instrs[i].op != IR_OP_COUNT) alive++;
    }
    ASSERT(alive == 2);  // CONST_INT r0 and ADD r2 survive

    ir_module_free(&mod);
}

static void test_opt_dce_keeps_side_effects(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    IrOperand imm42 = { .kind = IR_OP_IMM, .imm = 42 };
    // r0 = 42 (unused but PRINT has side effects)
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm42, 1, 0);
    IrOperand print_op = { .kind = IR_OP_REG, .reg = r0 };
    ir_emit_instr(&func, IR_PRINT, -1, (IrType){ .kind = IR_VOID, .semantic = NULL }, &print_op, 1, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_dce(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 0);  // nothing removed

    ir_module_free(&mod);
}

static void test_opt_dce_keeps_used_values(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    IrOperand imm3 = { .kind = IR_OP_IMM, .imm = 3 };
    ir_emit_instr(&func, IR_CONST_INT, r0, i64, &imm3, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r0 }
    };
    ir_emit_instr(&func, IR_ADD, r1, i64, ops, 2, 0);
    IrOperand ret_op = { .kind = IR_OP_REG, .reg = r1 };
    ir_emit_instr(&func, IR_RET, -1, (IrType){ .kind = IR_VOID, .semantic = NULL }, &ret_op, 1, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_dce(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 0);  // all values are used

    ir_module_free(&mod);
}
```

Add calls in `main()`:

```c
test_opt_dce_removes_unused_register();
test_opt_dce_keeps_side_effects();
test_opt_dce_keeps_used_values();
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make clean && make && make test-unit`
Expected: FAIL — stub returns 0 changes

- [ ] **Step 3: Implement DCE**

Replace `src/opt/opt_dce.c`:

```c
// src/opt/opt_dce.c
#include <stdlib.h>
#include <string.h>
#include "../include/opt.h"

static bool has_side_effects(IrOp op) {
    switch (op) {
    case IR_CALL:
    case IR_PRINT:
    case IR_STORE:
    case IR_RET:
    case IR_BR:
    case IR_CBR:
        return true;
    default:
        return false;
    }
}

OptResult opt_dce(IrModule *mod, IrFunction *func) {
    (void)mod;
    OptResult result = { .changes = 0, .modified = false };

    int use_count[func->next_reg];
    memset(use_count, 0, sizeof(use_count));

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        for (int j = 0; j < instr->operand_count; j++) {
            if (instr->operands[j].kind == IR_OP_REG) {
                int reg = instr->operands[j].reg;
                if (reg >= 0 && reg < func->next_reg) {
                    use_count[reg]++;
                }
            }
        }
    }

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        if (instr->dst >= 0 && use_count[instr->dst] == 0 &&
            !has_side_effects(instr->op)) {
            instr->op = IR_OP_COUNT;  // sentinel: dead instruction
            result.changes++;
            result.modified = true;
        }
    }

    return result;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make clean && make && make test-unit`
Expected: PASS — all 3 DCE tests pass

- [ ] **Step 5: Commit**

```bash
git add src/opt/opt_dce.c tests/unit/test_main.c
git commit -m "feat(opt): dead code elimination removes unused registers"
```

---

### Task 5: Algebraic Simplification Pass

**Files:**
- Create: `src/opt/opt_algebraic.c` (replace stub)
- Modify: `tests/unit/test_main.c`

- [ ] **Step 1: Write failing tests for algebraic simplification**

Add to `tests/unit/test_main.c` before `main()`:

```c
static void test_opt_algebraic_add_zero(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    // r0 = param (simulated as a register with no const definition)
    // r1 = 0
    IrOperand imm0 = { .kind = IR_OP_IMM, .imm = 0 };
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm0, 1, 0);
    // r2 = r0 + r1 (x + 0 -> x)
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_ADD, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    // First run constfold to resolve r1=0 as immediate, then algebraic
    opt_constfold(&mod, &mod.funcs[0]);
    OptResult r = opt_algebraic(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    IrInstr *add_instr = &mod.funcs[0].instrs[2];
    ASSERT(add_instr->op == IR_COPY);
    ASSERT(add_instr->operands[0].kind == IR_OP_REG);
    ASSERT(add_instr->operands[0].reg == r0);

    ir_module_free(&mod);
}

static void test_opt_algebraic_mul_one(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm1 = { .kind = IR_OP_IMM, .imm = 1 };
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm1, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_MUL, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    opt_constfold(&mod, &mod.funcs[0]);
    OptResult r = opt_algebraic(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    ASSERT(mod.funcs[0].instrs[2].op == IR_COPY);

    ir_module_free(&mod);
}

static void test_opt_algebraic_mul_zero(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r1 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand imm0 = { .kind = IR_OP_IMM, .imm = 0 };
    ir_emit_instr(&func, IR_CONST_INT, r1, i64, &imm0, 1, 0);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r1 }
    };
    ir_emit_instr(&func, IR_MUL, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    opt_constfold(&mod, &mod.funcs[0]);
    OptResult r = opt_algebraic(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    ASSERT(mod.funcs[0].instrs[2].op == IR_CONST_INT);
    ASSERT(mod.funcs[0].instrs[2].operands[0].imm == 0);

    ir_module_free(&mod);
}

static void test_opt_algebraic_xor_self(void) {
    IrFunction func = make_test_func();
    IrType i64 = { .kind = IR_I64, .semantic = NULL };
    int r0 = ir_new_reg(&func);
    int r2 = ir_new_reg(&func);
    IrOperand ops[2] = {
        { .kind = IR_OP_REG, .reg = r0 },
        { .kind = IR_OP_REG, .reg = r0 }
    };
    ir_emit_instr(&func, IR_BIT_XOR, r2, i64, ops, 2, 0);
    func.blocks[0].instr_end = func.instr_count;

    IrModule mod = make_single_func_module(func);
    OptResult r = opt_algebraic(&mod, &mod.funcs[0]);

    ASSERT(r.changes == 1);
    ASSERT(mod.funcs[0].instrs[0].op == IR_CONST_INT);
    ASSERT(mod.funcs[0].instrs[0].operands[0].imm == 0);

    ir_module_free(&mod);
}
```

Add calls in `main()`:

```c
test_opt_algebraic_add_zero();
test_opt_algebraic_mul_one();
test_opt_algebraic_mul_zero();
test_opt_algebraic_xor_self();
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make clean && make && make test-unit`
Expected: FAIL — stub returns 0 changes

- [ ] **Step 3: Implement algebraic simplification**

Replace `src/opt/opt_algebraic.c`:

```c
// src/opt/opt_algebraic.c
#include <stdlib.h>
#include <string.h>
#include "../include/opt.h"

static bool is_int_imm_reg(IrFunction *func, IrOperand *op, long long *out) {
    if (op->kind == IR_OP_IMM) {
        *out = op->imm;
        return true;
    }
    if (op->kind == IR_OP_REG) {
        for (int i = 0; i < func->instr_count; i++) {
            if (func->instrs[i].dst == op->reg &&
                func->instrs[i].op == IR_CONST_INT) {
                *out = func->instrs[i].operands[0].imm;
                return true;
            }
        }
    }
    return false;
}

static void replace_with_copy(IrInstr *instr, int src_reg) {
    instr->op = IR_COPY;
    instr->operands[0].kind = IR_OP_REG;
    instr->operands[0].reg = src_reg;
    instr->operand_count = 1;
}

static void replace_with_int(IrInstr *instr, long long val) {
    instr->op = IR_CONST_INT;
    instr->operands[0].kind = IR_OP_IMM;
    instr->operands[0].imm = val;
    instr->operand_count = 1;
}

OptResult opt_algebraic(IrModule *mod, IrFunction *func) {
    (void)mod;
    OptResult result = { .changes = 0, .modified = false };

    for (int i = 0; i < func->instr_count; i++) {
        IrInstr *instr = &func->instrs[i];
        if (instr->op == IR_OP_COUNT) continue;  // skip dead

        long long a, b;

        switch (instr->op) {
        case IR_ADD:
            if (instr->operand_count == 2) {
                if (is_int_imm_reg(func, &instr->operands[1], &b) && b == 0) {
                    replace_with_copy(instr, instr->operands[0].reg);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[0], &a) && a == 0) {
                    replace_with_copy(instr, instr->operands[1].reg);
                    result.changes++;
                    result.modified = true;
                }
            }
            break;
        case IR_SUB:
            if (instr->operand_count == 2 &&
                is_int_imm_reg(func, &instr->operands[1], &b) && b == 0) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_MUL:
            if (instr->operand_count == 2) {
                if (is_int_imm_reg(func, &instr->operands[1], &b) && b == 1) {
                    replace_with_copy(instr, instr->operands[0].reg);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[0], &a) && a == 1) {
                    replace_with_copy(instr, instr->operands[1].reg);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[1], &b) && b == 0) {
                    replace_with_int(instr, 0);
                    result.changes++;
                    result.modified = true;
                } else if (is_int_imm_reg(func, &instr->operands[0], &a) && a == 0) {
                    replace_with_int(instr, 0);
                    result.changes++;
                    result.modified = true;
                }
            }
            break;
        case IR_DIV:
            if (instr->operand_count == 2 &&
                is_int_imm_reg(func, &instr->operands[1], &b) && b == 1) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_AND:
            if (instr->operand_count == 2 &&
                instr->operands[0].kind == IR_OP_REG &&
                instr->operands[1].kind == IR_OP_REG &&
                instr->operands[0].reg == instr->operands[1].reg) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_OR:
            if (instr->operand_count == 2 &&
                instr->operands[0].kind == IR_OP_REG &&
                instr->operands[1].kind == IR_OP_REG &&
                instr->operands[0].reg == instr->operands[1].reg) {
                replace_with_copy(instr, instr->operands[0].reg);
                result.changes++;
                result.modified = true;
            }
            break;
        case IR_BIT_XOR:
            if (instr->operand_count == 2 &&
                instr->operands[0].kind == IR_OP_REG &&
                instr->operands[1].kind == IR_OP_REG &&
                instr->operands[0].reg == instr->operands[1].reg) {
                replace_with_int(instr, 0);
                result.changes++;
                result.modified = true;
            }
            break;
        default:
            break;
        }
    }

    return result;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make clean && make && make test-unit`
Expected: PASS — all 4 algebraic tests pass, plus all previous tests still pass

- [ ] **Step 5: Commit**

```bash
git add src/opt/opt_algebraic.c tests/unit/test_main.c
git commit -m "feat(opt): algebraic simplification (add 0, mul 1, mul 0, xor self)"
```

---

### Task 6: CLI Integration and End-to-End Test

**Files:**
- Modify: `src/main.c`
- Create: `tests/opt.sh`
- Modify: `Makefile`

- [ ] **Step 1: Add --opt-level flag parsing to main.c**

In `src/main.c`, in the `build` subcommand handler (around line 1188), add `opt_level` variable and parsing:

Find:
```c
const char *optimization = "-Os";
```

Add after it:
```c
int opt_level = 1;
```

Find the flag parsing loop and add before the `} else if (argv[i][0] != '-') {` line:
```c
} else if (strncmp(argv[i], "--opt-level=", 12) == 0) {
    opt_level = argv[i][12] - '0';
    if (opt_level < 0 || opt_level > 2) {
        fprintf(stderr, "error: --opt-level must be 0, 1, or 2\n");
        return 1;
    }
```

In the `build_qbe()` function (around line 340), after `ir_lower()` and before `emit_qbe()`, add:
```c
if (opt_level > 0) opt_run_pipeline(&module, opt_level);
```

In the `build_wasm()` function (around line 723), after `ir_lower()` and before `emit_wasm()`, add:
```c
if (opt_level > 0) opt_run_pipeline(&module, opt_level);
```

In the `dump_ir()` function (around line 835), after `ir_lower()` and before `ir_dump()`, add:
```c
if (opt_level > 0) opt_run_pipeline(&module, opt_level);
```

Add `#include "../include/opt.h"` at the top of `main.c` with the other includes.

Add to the `usage()` function:
```c
fprintf(stderr, "  --opt-level=0|1|2  Optimization level (default: 1)\n");
```

- [ ] **Step 2: Build and verify compilation**

Run: `make clean && make`
Expected: clean build, no warnings

- [ ] **Step 3: Create integration test script**

Create `tests/opt.sh`:

```bash
#!/bin/sh
# Integration test for IR optimization passes (--opt-level).
set -e
TIQ="${TIQ:-./build/tiq}"
PASS=0
FAIL=0

run_test() {
    name="$1"; src="$2"; expect="$3"; shift 3
    extra="$@"
    tmp=$(mktemp /tmp/tiq_opt_XXXXXX.tiq)
    out=$(mktemp /tmp/tiq_opt_XXXXXX.out)
    printf '%s\n' "$src" > "$tmp"
    if $TIQ build "$tmp" -o "${tmp}.bin" $extra 2>/dev/null && "${tmp}.bin" > "$out" 2>/dev/null; then
        actual=$(cat "$out")
        if [ "$actual" = "$expect" ]; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            echo "FAIL $name: expected '$expect', got '$actual'"
        fi
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name: compilation or execution failed"
    fi
    rm -f "$tmp" "${tmp}.bin" "$out"
}

# Constant folding: 2 + 3 should still produce 5
run_test "constfold_add" \
    'fn main() -> i64 { let x = 2 + 3; print(x); return 0 }' \
    "5"

# Algebraic: x + 0 should produce x
run_test "algebraic_add_zero" \
    'fn main() -> i64 { let x: i64 = 10; let y = x + 0; print(y); return 0 }' \
    "10"

# Algebraic: x * 1 should produce x
run_test "algebraic_mul_one" \
    'fn main() -> i64 { let x: i64 = 7; let y = x * 1; print(y); return 0 }' \
    "7"

# DCE: unused binding should not affect output
run_test "dce_unused" \
    'fn main() -> i64 { let dead = 999; let x = 42; print(x); return 0 }' \
    "42"

# opt-level=0 should still work (no optimization)
run_test "opt_level_0" \
    'fn main() -> i64 { let x = 2 + 3; print(x); return 0 }' \
    "5" \
    --opt-level=0

# Nested constant folding: (2 + 3) * (10 - 4) = 30
run_test "nested_constfold" \
    'fn main() -> i64 { let x = (2 + 3) * (10 - 4); print(x); return 0 }' \
    "30"

echo ""
echo "opt: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
```

- [ ] **Step 4: Add test to Makefile**

Add to the `test` target in `Makefile`:
```makefile
sh tests/opt.sh
```

- [ ] **Step 5: Run the integration test**

Run: `make && sh tests/opt.sh`
Expected: all 6 tests pass

- [ ] **Step 6: Run full test suite to verify no regressions**

Run: `make test`
Expected: all tests pass

- [ ] **Step 7: Commit**

```bash
git add src/main.c tests/opt.sh Makefile
git commit -m "feat(opt): integrate optimization pipeline with --opt-level CLI flag"
```

---

### Task 7: Verify Performance Impact

**Files:**
- None (measurement only)

- [ ] **Step 1: Record baseline with opt-level=0**

Run: `./build/tiq build examples/hello.tiq -o /tmp/hello_o0 --opt-level=0 && ls -l /tmp/hello_o0`

Run: `./build/tiq build examples/math_demo.tiq -o /tmp/math_o0 --opt-level=0 && ls -l /tmp/math_o0`

Record binary sizes.

- [ ] **Step 2: Build with opt-level=1 and compare**

Run: `./build/tiq build examples/hello.tiq -o /tmp/hello_o1 --opt-level=1 && ls -l /tmp/hello_o1`

Run: `./build/tiq build examples/math_demo.tiq -o /tmp/math_o1 --opt-level=1 && ls -l /tmp/math_o1`

- [ ] **Step 3: Compare binary sizes and verify correctness**

Run the opt-level=1 binaries and verify they produce identical output to opt-level=0:

```bash
./build/tiq build examples/math_demo.tiq -o /tmp/math_o0 --opt-level=0
/tmp/math_o0 > /tmp/out_o0.txt
./build/tiq build examples/math_demo.tiq -o /tmp/math_o1 --opt-level=1
/tmp/math_o1 > /tmp/out_o1.txt
diff /tmp/out_o0.txt /tmp/out_o1.txt
```

Expected: no diff (optimizations preserve semantics)

- [ ] **Step 4: Commit baseline update**

If binary sizes changed, update `docs/OPTIMIZATION_PLAN.md` with new numbers.

```bash
git add docs/OPTIMIZATION_PLAN.md
git commit -m "docs(opt): update performance baseline with Phase 1 optimization results"
```

(Only commit if the file was actually changed.)

---

## Self-Review Checklist

**Spec coverage:**
- [x] Pass interface (`OptResult`, `OptPassFn`) — Task 1
- [x] Pass manager with fixed-point iteration — Task 2
- [x] Constant folding (integer, float, bool, comparison, bitwise, CBR) — Task 3
- [x] Dead code elimination (unused registers, side-effect preservation) — Task 4
- [x] Algebraic simplification (add 0, sub 0, mul 1, mul 0, div 1, and/or/xor self) — Task 5
- [x] CLI integration (`--opt-level=0|1|2`) — Task 6
- [x] Integration tests — Task 6
- [x] Performance measurement — Task 7

**Placeholder scan:** No TBD/TODO found. All code blocks are complete.

**Type consistency:** `OptResult`, `OptPassFn`, `opt_run_pipeline`, `opt_constfold`, `opt_dce`, `opt_algebraic` — consistent across all tasks. `make_test_func()` and `make_single_func_module()` helpers defined once, used throughout.
