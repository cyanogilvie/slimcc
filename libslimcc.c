// libslimcc: library entry point for using slimcc as an in-process
// C-to-MIR JIT frontend. Replaces main.c in the library build: provides
// the driver globals and helpers that the compiler proper references, and
// drives the cc1 pipeline with in-memory input, captured diagnostics, and
// longjmp-based error recovery.
#include "slimcc.h"
#include "codegen-mir.h"
#include "libslimcc.h"
#include <setjmp.h>

//
// Globals normally defined by main.c. Defaults follow the CLI driver
// except that the language level is gnu23 with TS 25755 defer enabled,
// matching the library's purpose.
//

StringArray include_paths;
StringArray iquote_paths;
StringArray display_files;
bool opt_fcommon;
int opt_fpic;
int opt_fpie;
bool opt_femulated_tls;
int opt_fn_align = 1;
bool opt_use_plt = true;
bool opt_optimize = true;
bool opt_reuse_stack = true;
bool opt_g;
bool opt_func_sections;
bool opt_data_sections;
bool opt_werror;
bool opt_cc1_asm_pp;
const char *opt_visibility;
StdVer opt_std = STD_C23;
bool is_iso_std;
bool opt_fdefer_ts = true;
bool opt_short_enums;
bool opt_gnu_keywords;
bool opt_gnu89_inline;
bool opt_ms_anon_struct;
bool opt_disable_visibility;
bool opt_fake_always_inline;

bool opt_E;
bool opt_dM;
bool opt_pie;
bool opt_nopie;
bool opt_pthread;
bool opt_r;
bool opt_rdynamic;
bool opt_static;
bool opt_static_pie;
bool opt_static_libgcc;
bool opt_shared;
bool opt_s;
bool opt_nostartfiles;
bool opt_nodefaultlibs;
bool opt_nolibc;
const char *default_ld = "ld";
const char *default_as = "as";
const char *dumpmachine_str;
char *argv0 = "libslimcc";

//
// Compile-in-progress state
//

static jmp_buf compile_jmp;
static bool compile_active;
static char *diag_buf;
static size_t diag_len;

//
// main.c helpers referenced from the compiler proper
//

void cleanup_exit(int status) {
  if (compile_active)
    longjmp(compile_jmp, status ? status : 1);
  exit(status);
}

bool file_exists(const char *path) {
  if (slimcc_vfile_get(path))
    return true;
  struct stat st;
  return !stat(path, &st);
}

bool in_sysincl_path(int idx) {
  (void)idx;
  return false;
}

bool ignore_missing_dep(const char *path, const char *filename, Token *tok) {
  if (!path) {
    if (tok)
      error_tok(tok, "file not found");
    error("`%s` file not found", filename);
  }
  return false;
}

//
// Input-file dependency recording (for precompiled-preamble validation).
// add_dep_file() is already invoked by the preprocessor for every file it
// pulls in (the base file, -include files, and each #include'd header). When
// a recorder is active it captures the path, mtime and size of every real
// (non-vfile) file, so a cached preamble can later be revalidated against the
// filesystem. Inactive (dep_rec == NULL) during ordinary compiles.
//
typedef struct {
  char *path;
  struct timespec mtim;
  off_t size;
} DepEntry;

typedef struct {
  DepEntry *data;
  int len, cap;
} DepList;

static DepList *dep_rec;

static void dep_list_free(DepList *d) {
  if (!d)
    return;
  for (int i = 0; i < d->len; i++)
    free(d->data[i].path);
  free(d->data);
}

// True iff every recorded file still exists with the same mtime and size.
static bool dep_list_unchanged(const DepList *d) {
  for (int i = 0; i < d->len; i++) {
    struct stat st;
    if (stat(d->data[i].path, &st))
      return false;
    if (st.st_mtim.tv_sec != d->data[i].mtim.tv_sec ||
        st.st_mtim.tv_nsec != d->data[i].mtim.tv_nsec ||
        st.st_size != d->data[i].size)
      return false;
  }
  return true;
}

void add_dep_file(const char *path, bool is_sys) {
  (void)is_sys;
  if (!dep_rec || slimcc_vfile_get(path))
    return; // recording off, or an in-memory file with no on-disk mtime
  for (int i = 0; i < dep_rec->len; i++)
    if (!strcmp(dep_rec->data[i].path, path))
      return; // a guard-less header included twice records once
  struct stat st;
  if (stat(path, &st))
    return; // unreadable now: skip; a genuine miss is caught at revalidation
  if (dep_rec->len == dep_rec->cap) {
    dep_rec->cap = dep_rec->cap ? dep_rec->cap * 2 : 16;
    dep_rec->data = realloc(dep_rec->data, dep_rec->cap * sizeof *dep_rec->data);
  }
  DepEntry *e = &dep_rec->data[dep_rec->len++];
  e->path = strdup(path);
  e->mtim = st.st_mtim;
  e->size = st.st_size;
}

//
// Precompiled preamble (header cache). slimcc_pch_create captures the cache
// key, the validated file-dependency set, and a snapshot (via pp_snapshot) of
// the preamble's macro/guard tables + preprocessed token chain; slimcc_compile
// installs that snapshot to skip re-tokenizing the header closure.
//
struct slimcc_pch {
  uint64_t key;   // hash of (defines, include paths) this pch was built with
  char *preamble; // the preamble text, for the inline fallback path
  DepList deps;   // real files read while building, for mtime revalidation
  Arena arena;    // persistent storage for pp (kept alive for the pch's life)
  PchState *pp;   // snapshotted macro/guard tables + preamble token chain
};

static uint64_t fnv1a_str(uint64_t h, const char *s) {
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 0x100000001b3ull;
  }
  return (h ^ ',') * 0x100000001b3ull; // separator so {"ab"} != {"a","b"}
}

// The key only guards option compatibility (a compile must use the same
// defines/include paths the pch was built with); the preamble text is the
// caller's responsibility to keep paired, and header edits are caught by the
// mtime check. Hashed in the given order — a reorder just misses, never a
// false hit. slimcc_compile recomputes this from opt and compares.
static uint64_t pch_compute_key(const slimcc_options *opt) {
  uint64_t h = 0xcbf29ce484222325ull;
  if (opt) {
    for (int i = 0; i < opt->n_defines; i++)
      h = fnv1a_str(h, opt->defines[i]);
    for (int i = 0; i < opt->n_include_paths; i++)
      h = fnv1a_str(h, opt->include_paths[i]);
  }
  return h;
}

void add_include_path(StringArray *arr, const char *s) {
  strarray_push(arr, s);
}

char *find_dir_w_file(const char *pattern) {
  (void)pattern;
  return NULL;
}

void run_subprocess(const char **argv) {
  (void)argv;
  error("subprocesses are not available in library mode");
}

//
// MIR error routing: format into the diagnostics stream, then unwind.
//

static void NORETURN mir_error(MIR_error_type_t type, const char *fmt, ...) {
  FILE *f = slimcc_diag_file ? slimcc_diag_file : stderr;
  va_list ap;
  va_start(ap, fmt);
  fprintf(f, "MIR error %d: ", (int)type);
  vfprintf(f, fmt, ap);
  fprintf(f, "\n");
  va_end(ap);
  cleanup_exit(1);
}

static void lib_macros(void) {
  switch (opt_std) {
  case STD_C94: define_macro("__STDC_VERSION__", "199409L"); break;
  case STD_C99: define_macro("__STDC_VERSION__", "199901L"); break;
  case STD_C11: define_macro("__STDC_VERSION__", "201112L"); break;
  case STD_C17: define_macro("__STDC_VERSION__", "201710L"); break;
  case STD_C23: define_macro("__STDC_VERSION__", "202311L"); break;
  default: break;
  }
  if (opt_std >= STD_C99)
    define_macro("__GNUC_STDC_INLINE__", "1");

  define_macro("__STDC_UTF_16__", "1");
  define_macro("__STDC_UTF_32__", "1");
  define_macro("__STDC_DEFER_TS25755__", opt_fdefer_ts ? "2" : "1");
  define_macro("_REENTRANT", "1");
}

// Reset all compiler state. Called before each compile and after a failed
// one, so a bad translation unit cannot poison the next.
static void reset_all(void) {
  tokenize_reset();
  preprocess_reset();
  parse_reset();
  type_reset();
  codegen_mir_reset();
  free(include_paths.data);
  free(iquote_paths.data);
  free(display_files.data);
  include_paths = (StringArray){0};
  iquote_paths = (StringArray){0};
  display_files = (StringArray){0};
}

// The compiler's own headers (stdarg.h, stdatomic.h, bitint_builtins,
// ...) travel inside the library as virtual files under "<slimcc>",
// which is searched before any caller-supplied include path - the same
// precedence the CLI gives its builtin header directory. The parser
// injects bitint_builtins from there into every translation unit.
// SLIMCC_HEADERS_INC lets out-of-tree builds (meson) point at their
// generated copy so a stale in-tree one can't shadow it.
#ifndef SLIMCC_HEADERS_INC
#define SLIMCC_HEADERS_INC "libslimcc-headers.inc"
#endif
#include SLIMCC_HEADERS_INC

static void register_embedded_headers(void) {
  char name[64];
  for (size_t i = 0; i < sizeof(embedded_headers) / sizeof(*embedded_headers); i++) {
    snprintf(name, sizeof(name), "<slimcc>/%s", embedded_headers[i].name);
    slimcc_vfile_add(name, embedded_headers[i].contents);
  }
  add_include_path(&include_paths, "<slimcc>");
}

static void arenas_off(void) {
  if (ast_arena.cur)
    arena_off(&ast_arena);
  if (pp_arena.cur)
    arena_off(&pp_arena);
  if (cc1_arena.cur)
    arena_off(&cc1_arena);
}

MIR_module_t slimcc_compile(MIR_context_t ctx, const char *name, const char *source,
                            const slimcc_options *opt, char **errmsg) {
  if (errmsg)
    *errmsg = NULL;

  slimcc_lib_mode = true;
  // Per-compile state is cleared eagerly at the end of each compile (success
  // and failure paths both finish with reset_all), so on entry it is already
  // clean — the very first call starts from zero-initialized globals. This
  // avoids retaining one compile's worth of token pool / file contents / maps
  // idle between compiles (or until shutdown), which matters on memory-
  // constrained hosts.

  diag_buf = NULL;
  slimcc_diag_file = open_memstream(&diag_buf, &diag_len);

  // The whole pipeline, MIR build errors included, unwinds to here. On
  // failure the scratch context holds a half-built function/module; the
  // staged teardown closes those before destroying it, tolerating MIR
  // errors raised by the teardown itself (each re-entry advances a stage;
  // if even MIR_finish fails the context is leaked rather than corrupted).
  static volatile int fail_stage;
  MIR_context_t scratch = MIR_init();
  MIR_set_error_func(scratch, mir_error);

  if (setjmp(compile_jmp)) {
    switch (fail_stage) {
    case 0:
      fail_stage = 1;
      codegen_mir_abort(); /* may longjmp back here */
      /* fallthrough */
    case 1:
      fail_stage = 2;
      MIR_finish(scratch); /* may longjmp back here */
      /* fallthrough */
    default:
      break;
    }
    compile_active = false;
    parse_free_scopes(); // nested Scopes live in the still-on AST arena
    arenas_off();
    fclose(slimcc_diag_file);
    slimcc_diag_file = NULL;
    if (errmsg)
      *errmsg = diag_buf;
    else
      free(diag_buf);
    diag_buf = NULL;
    reset_all();
    return NULL;
  }
  fail_stage = 0;
  compile_active = true;

  slimcc_vfile_add(name, source);
  register_embedded_headers();
  if (opt) {
    for (int i = 0; i < opt->n_vfiles; i++)
      slimcc_vfile_add(opt->vfiles[i].name, opt->vfiles[i].contents);
    for (int i = 0; i < opt->n_include_paths; i++)
      add_include_path(&include_paths, opt->include_paths[i]);
  }

  arena_on(&cc1_arena);
  arena_on(&pp_arena);

  codegen_mir_begin(scratch, name);

  const slimcc_pch *pch = opt ? opt->pch : NULL;
  bool use_pch = pch && pch->key == pch_compute_key(opt) && slimcc_pch_valid(pch);
  Token *tok;

  if (use_pch) {
    // Fast path: the preamble's macros, include guards and preprocessed token
    // chain come from the pch; only the cdef body (source) is tokenized here,
    // then spliced after the preamble. init_macros/platform_init_cc1/lib_macros
    // and the CLI defines are all already baked into the pch (the key check
    // guarantees matching defines/paths). The non-macro target type globals
    // (ty_size_t, enum_ty, ...) that platform_init_cc1 sets via init_ty_lp64
    // are process-persistent (type_reset doesn't clear them) and were set when
    // the pch was built, so they need no re-init here — and must NOT be, since
    // re-running init_ty_lp64 would re-#define its macros and churn the table.
    Token *pre = pp_install(pch->pp);
    Token *body = preprocess(name, &(StringArray){0}, &(StringArray){0});
    if (pre) {
      Token *t = pre;
      while (t->next)
        t = t->next;
      t->next = body;
      tok = pre;
    } else {
      tok = body;
    }
  } else {
    init_macros();
    platform_init_cc1();
    lib_macros();
    if (opt)
      for (int i = 0; i < opt->n_defines; i++)
        define_macro_cli(opt->defines[i]);

    if (pch) {
      // A pch was supplied but is unusable (stale headers or mismatched
      // options): fall back to compiling preamble+body as one source, so the
      // body still sees the headers. A pch is purely an optimization, never a
      // correctness dependency. (tokenize copies the vfile contents up front,
      // so freeing `combined` right after preprocess is safe; a compile error
      // longjmps past the free, an accepted leak on this doubly-rare path.)
      size_t pl = strlen(pch->preamble), sl = strlen(source);
      char *combined = malloc(pl + 1 + sl + 1);
      memcpy(combined, pch->preamble, pl);
      combined[pl] = '\n';
      memcpy(combined + pl + 1, source, sl + 1);
      slimcc_vfile_add(name, combined); // overwrites the body-only registration
      tok = preprocess(name, &(StringArray){0}, &(StringArray){0});
      free(combined);
    } else {
      tok = preprocess(name, &(StringArray){0}, &(StringArray){0});
    }
  }
  tok = prepare_parse(tok);
  arena_off(&pp_arena);

  Obj *prog = parse(tok);
  codegen(prog, NULL);

  arena_off(&cc1_arena);

  MIR_module_t mod = codegen_mir_result();
  if (opt && opt->mir_dump)
    MIR_output_module(scratch, opt->mir_dump, mod);
  MIR_change_module_ctx(scratch, mod, ctx);
  MIR_finish(scratch);
  compile_active = false;

  fclose(slimcc_diag_file);
  slimcc_diag_file = NULL;
  free(diag_buf); // warnings only; surfaced via a callback in a later version
  diag_buf = NULL;
  // Free this compile's per-compile state now rather than retaining it until
  // the next compile. The finished module lives in the caller's ctx and shares
  // nothing with it. (The error path above resets symmetrically before NULL.)
  reset_all();
  return mod;
}

slimcc_pch *slimcc_pch_create(const char *preamble, const slimcc_options *opt,
                              char **errmsg) {
  if (errmsg)
    *errmsg = NULL;
  slimcc_lib_mode = true;

  diag_buf = NULL;
  slimcc_diag_file = open_memstream(&diag_buf, &diag_len);

  DepList deps = {0};

  // Preprocess-only: no MIR context or codegen, so a preprocessor error just
  // unwinds here (mirrors slimcc_compile's recovery, minus the codegen stages).
  if (setjmp(compile_jmp)) {
    compile_active = false;
    dep_rec = NULL;
    parse_free_scopes();
    arenas_off();
    fclose(slimcc_diag_file);
    slimcc_diag_file = NULL;
    if (errmsg)
      *errmsg = diag_buf;
    else
      free(diag_buf);
    diag_buf = NULL;
    dep_list_free(&deps);
    reset_all();
    return NULL;
  }
  compile_active = true;

  static const char *pch_name = "<preamble>";
  slimcc_vfile_add(pch_name, preamble);
  register_embedded_headers();
  if (opt) {
    for (int i = 0; i < opt->n_vfiles; i++)
      slimcc_vfile_add(opt->vfiles[i].name, opt->vfiles[i].contents);
    for (int i = 0; i < opt->n_include_paths; i++)
      add_include_path(&include_paths, opt->include_paths[i]);
  }

  arena_on(&cc1_arena);
  arena_on(&pp_arena);

  init_macros();
  platform_init_cc1();
  lib_macros();
  if (opt)
    for (int i = 0; i < opt->n_defines; i++)
      define_macro_cli(opt->defines[i]);

  // Every real file opened here is recorded (with mtime+size) via add_dep_file.
  // The macro table + token stream are fully populated once preprocess returns
  // and before prepare_parse would tear the macro table down — that is the
  // checkpoint we snapshot into the pch-owned arena.
  dep_rec = &deps;
  Token *pre = preprocess(pch_name, &(StringArray){0}, &(StringArray){0});
  dep_rec = NULL;

  slimcc_pch *pch = calloc(1, sizeof *pch);
  arena_on(&pch->arena); // never arena_off'd; freed by slimcc_pch_free
  pch->pp = pp_snapshot(&pch->arena, pre);

  arena_off(&pp_arena);
  arena_off(&cc1_arena);

  compile_active = false;
  fclose(slimcc_diag_file);
  slimcc_diag_file = NULL;
  free(diag_buf);
  diag_buf = NULL;

  pch->key = pch_compute_key(opt);
  pch->preamble = strdup(preamble);
  pch->deps = deps; // ownership transferred to the pch
  reset_all();
  return pch;
}

bool slimcc_pch_valid(const slimcc_pch *pch) {
  return pch && dep_list_unchanged(&pch->deps);
}

int slimcc_pch_nfiles(const slimcc_pch *pch) {
  return pch ? pch->deps.len : 0;
}

void slimcc_pch_free(slimcc_pch *pch) {
  if (!pch)
    return;
  pp_free_state(pch->pp);
  arena_off(&pch->arena); // release/recycle the persistent pool chain
  free(pch->preamble);
  dep_list_free(&pch->deps);
  free(pch);
}

// Atomic helpers from slimcc-mir-helpers.c.
extern bool __slimcc_jit_cas_1(void *, void *, uint8_t);
extern bool __slimcc_jit_cas_2(void *, void *, uint16_t);
extern bool __slimcc_jit_cas_4(void *, void *, uint32_t);
extern bool __slimcc_jit_cas_8(void *, void *, uint64_t);
extern uint8_t __slimcc_jit_exch_1(void *, uint8_t);
extern uint16_t __slimcc_jit_exch_2(void *, uint16_t);
extern uint32_t __slimcc_jit_exch_4(void *, uint32_t);
extern uint64_t __slimcc_jit_exch_8(void *, uint64_t);
extern void __slimcc_jit_fence(void);
extern void *__slimcc_emutls_get_address(void *);
extern void slimcc_register_bitint_helpers(MIR_context_t ctx);

void slimcc_register_helpers(MIR_context_t ctx) {
  slimcc_register_bitint_helpers(ctx);
  MIR_load_external(ctx, "memset", (void *)memset);
  MIR_load_external(ctx, "memcpy", (void *)memcpy);
  MIR_load_external(ctx, "__slimcc_jit_cas_1", (void *)__slimcc_jit_cas_1);
  MIR_load_external(ctx, "__slimcc_jit_cas_2", (void *)__slimcc_jit_cas_2);
  MIR_load_external(ctx, "__slimcc_jit_cas_4", (void *)__slimcc_jit_cas_4);
  MIR_load_external(ctx, "__slimcc_jit_cas_8", (void *)__slimcc_jit_cas_8);
  MIR_load_external(ctx, "__slimcc_jit_exch_1", (void *)__slimcc_jit_exch_1);
  MIR_load_external(ctx, "__slimcc_jit_exch_2", (void *)__slimcc_jit_exch_2);
  MIR_load_external(ctx, "__slimcc_jit_exch_4", (void *)__slimcc_jit_exch_4);
  MIR_load_external(ctx, "__slimcc_jit_exch_8", (void *)__slimcc_jit_exch_8);
  MIR_load_external(ctx, "__slimcc_jit_fence", (void *)__slimcc_jit_fence);
  MIR_load_external(ctx, "__slimcc_emutls_get_address", (void *)__slimcc_emutls_get_address);
}

void slimcc_shutdown(void) {
  // Per-compile state is already freed at the end of each compile; the one
  // thing that persists across compiles is the arena pool freelist that
  // arena_off keeps for reuse. Release it (plus a defensive reset_all() in case
  // shutdown is reached in an unexpected state). Intended for a final
  // library-mode shutdown (an embedder unloaded from a host that outlives it);
  // compiling again afterwards simply rebuilds everything on demand.
  reset_all();
  arena_free_pools();
}
