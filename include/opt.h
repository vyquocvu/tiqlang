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
