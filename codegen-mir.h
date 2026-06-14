// Interface between libslimcc.c and the MIR backend (codegen-mir.c).
#ifndef SLIMCC_CODEGEN_MIR_H
#define SLIMCC_CODEGEN_MIR_H

#include "mir.h"

// Open a new MIR module in ctx; subsequent parsing emits into it via the
// backend boundary (emit_text/codegen).
void codegen_mir_begin(MIR_context_t ctx, const char *module_name);

// The module finished by codegen(), or NULL if compilation did not finish.
MIR_module_t codegen_mir_result(void);

// Close any function/module left open by an error unwind so the scratch
// context can be destroyed. The MIR calls involved may themselves raise
// MIR errors; the caller's error handler must tolerate re-entry.
void codegen_mir_abort(void);

// Free the backend's module-lifetime symbol table and label map. Called at the
// end of each compile (via reset_all) so it is not retained between compiles.
void codegen_mir_reset(void);

#endif
