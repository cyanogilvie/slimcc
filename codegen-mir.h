// Interface between libslimcc.c and the MIR backend (codegen-mir.c).
#ifndef SLIMCC_CODEGEN_MIR_H
#define SLIMCC_CODEGEN_MIR_H

#include "mir.h"

// Open a new MIR module in ctx; subsequent parsing emits into it via the
// backend boundary (emit_text/codegen).
void codegen_mir_begin(MIR_context_t ctx, const char *module_name);

// The module finished by codegen(), or NULL if compilation did not finish.
MIR_module_t codegen_mir_result(void);

#endif
