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
  const struct slimcc_pch *pch; // if set, a precompiled preamble (see below)
  int debug; // if non-zero, emit source locations for debug info (see slimcc_debug_obj)
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

// --- JIT debug symbols (GDB JIT interface) -------------------------------
//
// MIR emits no DWARF, so JIT'd frames are anonymous to gdb/perf. To restore
// function-granularity symbolization (named frames in backtraces, `break
// funcname`, labeled disassembly) build a minimal in-memory ELF object that
// holds only a symbol table over the generated functions and hand it to a
// debugger through the GDB JIT interface (the __jit_debug_register_code
// protocol). Addresses and sizes come from MIR after code generation:
// item->u.func->machine_code and ->code_len.
typedef struct slimcc_jitsym {
  const char *name; // symbol name (referenced, not copied; outlive the call)
  const void *addr; // runtime address of the function/object
  size_t size;      // byte length (0 if unknown)
  int is_func;      // non-zero: STT_FUNC; zero: STT_OBJECT
  // Optional, functions only: the MIR per-function code-offset -> source-line
  // map (MIR_func.line_map / line_map_len, valid after MIR_gen). When present,
  // slimcc_debug_obj also emits DWARF .debug_line + a subprogram DIE so a
  // debugger can do source-level stepping. File ids in the map index slimcc's
  // debug file table (see slimcc_debug_intern_file).
  const MIR_line_map_t *line_map;
  size_t line_map_len;
  // Optional: the MIR_func_t (as void*) this symbol came from. When set (and the
  // module was generated in spill-all mode), slimcc_debug_obj also emits
  // DW_TAG_variable DIEs with frame-relative locations for the function's
  // locals, so a debugger can inspect them. The locals themselves are recorded
  // by the compiler via slimcc_debug_add_local.
  const void *mir_func;
} slimcc_jitsym;

// Build a minimal ELF object (ET_REL, host machine) holding a .symtab over the
// given symbols, anchored to one allocatable .text section spanning their
// address range — what gdb's JIT reader needs to map a PC to a JIT'd function
// name. Group symbols with nearby addresses per call (e.g. one object per JIT
// context's code region); a wide address span makes a correspondingly large
// section. On success returns 0 and sets *buf (malloc'd; free with free()) and
// *size. On failure returns -1 and, if errmsg is non-NULL, sets it to a
// malloc'd message. Pure ELF construction: touches no compiler or MIR state,
// so it needs no serialization.
int slimcc_debug_obj(const slimcc_jitsym *syms, int nsyms, void **buf,
                     size_t *size, char **errmsg);

// Clear the persistent debug source-file table that accumulates across the
// compiles of one debug build (see slimcc_debug_intern_file). Call after
// building the debug object for a cdef, before starting the next, so file ids
// don't leak between unrelated builds.
void slimcc_debug_reset(void);

// --- Precompiled preamble (header cache) ---------------------------------
//
// Compiling a cdef re-tokenizes the full header closure pulled in by the
// preamble (for jitc, tcl.h via tclstuff.h: ~12k lines across ~37 files)
// every time, which dominates the small-input compile latency. A slimcc_pch
// captures the preprocessed state of a fixed preamble once so subsequent
// compiles sharing it skip the re-tokenize.
//
// `preamble` is the text that conceptually precedes the cdef body, e.g.
// "#include <tclstuff.h>\n". Build a pch once with the same options
// (include paths, defines, vfiles) you pass to slimcc_compile, then set
// opt->pch and pass the cdef body alone as `source`.
//
// A pch records every real file its preamble preprocess opened, with mtime
// and size. slimcc_compile revalidates it (cache key + re-stat of those
// files) on each use: a stale or mismatched pch is silently ignored and the
// compile falls back to processing the preamble inline, so a pch is never a
// correctness hazard — at worst it costs the stat() calls.
//
// On failure returns NULL and, if errmsg is non-NULL, sets it to a malloc'd
// diagnostic the caller must free. Free with slimcc_pch_free. Serialize pch
// creation/use/free with slimcc_compile like all other library state.
typedef struct slimcc_pch slimcc_pch;

slimcc_pch *slimcc_pch_create(const char *preamble, const slimcc_options *opt,
                              char **errmsg);
void slimcc_pch_free(slimcc_pch *pch);

// Re-stat every file the pch read while building; returns false if any is
// missing or its mtime/size changed since. slimcc_compile calls this itself;
// it is exposed for callers that want to proactively rebuild a stale pch.
bool slimcc_pch_valid(const slimcc_pch *pch);

// Number of input files the pch captured (diagnostics / tests).
int slimcc_pch_nfiles(const slimcc_pch *pch);

// Release libslimcc's process-global state (the cross-compilation arena pool
// freelist). Optional: call once when unloading the library from a host that
// outlives it, so nothing is left allocated. Not required before process exit.
// Safe to keep compiling afterwards (state is lazily rebuilt). Serialize with
// slimcc_compile like everything else.
void slimcc_shutdown(void);

#endif
