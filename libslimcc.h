// libslimcc: embed slimcc as an in-process C-to-MIR JIT compiler frontend.
//
// Typical use:
//
//   MIR_context_t ctx = MIR_init();
//   char *err;
//   MIR_module_t m = slimcc_compile(ctx, "cdef", source, &opts, &err);
//   if (!m) { report(err); free(err); }
//   slimcc_register_helpers(ctx);          // before MIR_link
//   MIR_load_module(ctx, m);
//   MIR_link(ctx, MIR_set_gen_interface, resolver);
//
// NOT thread-safe: the compiler keeps global state, so callers must
// serialize calls to slimcc_compile (a single mutex around it suffices;
// the returned modules and the caller's MIR context are unaffected).
#ifndef LIBSLIMCC_H
#define LIBSLIMCC_H

#include <stdio.h>
#include "mir.h"

typedef struct slimcc_vfile {
  const char *name;     // path as seen by #include / the source name
  const char *contents; // NUL-terminated; must outlive all compilations
} slimcc_vfile;

typedef struct slimcc_options {
  const char **include_paths; // -I equivalents
  int n_include_paths;
  const char **defines; // "NAME" or "NAME=VALUE"
  int n_defines;
  const slimcc_vfile *vfiles; // in-memory #include targets
  int n_vfiles;
  FILE *mir_dump; // if set, the textual MIR module is dumped here
} slimcc_options;

// Compile one translation unit from memory. On success returns a finished,
// unloaded MIR module that has been migrated into ctx (the caller loads and
// links it). On failure returns NULL and, if errmsg is non-NULL, sets it to
// a malloc'd diagnostic string the caller must free.
//
// opt may be NULL. The compiler's predefined macros target the host
// architecture; no standard include paths are configured.
MIR_module_t slimcc_compile(MIR_context_t ctx, const char *name, const char *source,
                            const slimcc_options *opt, char **errmsg);

// Register the runtime helper functions that compiled code may reference
// (memset/memcpy for aggregate initialization, atomics, and the >64-bit
// _BitInt arithmetic helpers) with a MIR context via MIR_load_external.
// Call once per context before MIR_link, or provide the symbols through
// your own import resolver instead.
void slimcc_register_helpers(MIR_context_t ctx);

// Release libslimcc's process-global state (the cross-compilation arena pool
// freelist). Optional: call once when unloading the library from a host that
// outlives it, so nothing is left allocated. Not required before process exit.
// Safe to keep compiling afterwards (state is lazily rebuilt). Serialize with
// slimcc_compile like everything else.
void slimcc_shutdown(void);

#endif
