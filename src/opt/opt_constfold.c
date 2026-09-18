#include "../../include/opt.h"

OptResult opt_constfold(IrModule *mod, IrFunction *func) {
    (void)mod; (void)func;
    OptResult r = { .changes = 0, .modified = false };
    return r;
}
