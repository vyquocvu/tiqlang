#include "../../include/opt.h"

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
