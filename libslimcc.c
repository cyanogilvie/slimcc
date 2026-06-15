// libslimcc: library entry point for using slimcc as an in-process
// C-to-MIR JIT frontend. Replaces main.c in the library build: provides
// the driver globals and helpers that the compiler proper references, and
// drives the cc1 pipeline with in-memory input, captured diagnostics, and
// longjmp-based error recovery.
#include "slimcc.h"
#include "codegen-mir.h"
#include "libslimcc.h"
#include <setjmp.h>
#include <elf.h>

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
  opt_g = opt && opt->debug; // gate source-location stamping in codegen-mir
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
  slimcc_debug_reset();
}

// --- JIT debug symbols (GDB JIT interface) -------------------------------

// Host ELF machine for the emitted debug object. Both production targets are
// little-endian; the GDB JIT consumer is always this same process, so the
// object is naturally host-native.
#if defined(__x86_64__)
#define SLIMCC_ELF_MACHINE EM_X86_64
#elif defined(__aarch64__)
#define SLIMCC_ELF_MACHINE EM_AARCH64
#elif defined(__riscv) && __riscv_xlen == 64
#define SLIMCC_ELF_MACHINE EM_RISCV
#elif defined(__powerpc64__)
#define SLIMCC_ELF_MACHINE EM_PPC64
#elif defined(__s390x__)
#define SLIMCC_ELF_MACHINE EM_S390
#else
#define SLIMCC_ELF_MACHINE EM_NONE
#endif

// DWARF DW_OP_regN for the host frame pointer (DW_AT_frame_base): MIR keeps a
// frame pointer for debug functions and reports slot offsets relative to it.
#if defined(__aarch64__)
#define SLIMCC_DW_OP_FP DW_OP_reg29
#else
#define SLIMCC_DW_OP_FP DW_OP_reg6
#endif

static int debug_obj_fail(char **errmsg, const char *msg) {
  if (errmsg) *errmsg = strdup(msg);
  return -1;
}

// Persistent debug source-file table: file id (1-based) -> path. Accumulates
// across the compiles of one debug build and survives reset_all (unlike
// display_files, which is per-compile), so all of a cdef's code blocks share
// one id space — the ids the codegen stamped onto MIR insns stay meaningful
// when slimcc_debug_obj runs after every block has compiled. Cleared by
// slimcc_debug_reset().
static struct {
  char **names;
  int len, cap;
} dbg_files;

int slimcc_debug_intern_file(const char *name) {
  if (!name) return 0;
  for (int i = 0; i < dbg_files.len; i++)
    if (strcmp(dbg_files.names[i], name) == 0) return i + 1;
  if (dbg_files.len == dbg_files.cap) {
    dbg_files.cap = dbg_files.cap ? dbg_files.cap * 2 : 8;
    dbg_files.names = realloc(dbg_files.names, dbg_files.cap * sizeof(char *));
  }
  dbg_files.names[dbg_files.len++] = strdup(name);
  return dbg_files.len;
}

// ---- small growable byte buffer for assembling ELF/DWARF section bodies ----
typedef struct {
  unsigned char *p;
  size_t len, cap;
} Buf;

static int buf_reserve(Buf *b, size_t n) {
  if (b->len + n <= b->cap) return 0;
  size_t c = b->cap ? b->cap * 2 : 128;
  while (c < b->len + n) c *= 2;
  void *np = realloc(b->p, c);
  if (!np) return -1;
  b->p = np;
  b->cap = c;
  return 0;
}
static void buf_bytes(Buf *b, const void *d, size_t n) {
  if (buf_reserve(b, n)) return;
  memcpy(b->p + b->len, d, n);
  b->len += n;
}
static void buf_u8(Buf *b, uint8_t v) { buf_bytes(b, &v, 1); }
static void buf_u16(Buf *b, uint16_t v) { buf_bytes(b, &v, 2); }
static void buf_u32(Buf *b, uint32_t v) { buf_bytes(b, &v, 4); }
static void buf_u64(Buf *b, uint64_t v) { buf_bytes(b, &v, 8); }
static void buf_str(Buf *b, const char *s) { buf_bytes(b, s, strlen(s) + 1); }
static void buf_uleb(Buf *b, uint64_t v) {
  do {
    uint8_t x = v & 0x7f;
    v >>= 7;
    if (v) x |= 0x80;
    buf_u8(b, x);
  } while (v);
}
static void buf_sleb(Buf *b, int64_t v) {
  for (;;) {
    uint8_t x = v & 0x7f;
    v >>= 7; // arithmetic shift
    int done = (v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40));
    if (!done) x |= 0x80;
    buf_u8(b, x);
    if (done) break;
  }
}

// DWARF constants (just what we emit).
enum {
  DW_TAG_compile_unit = 0x11,
  DW_TAG_subprogram = 0x2e,
  DW_TAG_base_type = 0x24,
  DW_TAG_pointer_type = 0x0f,
  DW_TAG_variable = 0x34,
  DW_TAG_formal_parameter = 0x05,
  DW_TAG_structure_type = 0x13,
  DW_TAG_union_type = 0x17,
  DW_TAG_array_type = 0x01,
  DW_TAG_subrange_type = 0x21,
  DW_TAG_enumeration_type = 0x04,
  DW_TAG_enumerator = 0x28,
  DW_TAG_member = 0x0d,
  DW_TAG_typedef = 0x16,
  DW_TAG_const_type = 0x26,
  DW_TAG_volatile_type = 0x35,
  DW_TAG_unspecified_type = 0x3b,
  DW_TAG_subroutine_type = 0x15,
  DW_CHILDREN_no = 0,
  DW_CHILDREN_yes = 1,
  DW_AT_name = 0x03,
  DW_AT_byte_size = 0x0b,
  DW_AT_bit_size = 0x0d,
  DW_AT_encoding = 0x3e,
  DW_AT_location = 0x02,
  DW_AT_type = 0x49,
  DW_AT_frame_base = 0x40,
  DW_AT_data_member_location = 0x38,
  DW_AT_data_bit_offset = 0x6b,
  DW_AT_count = 0x37,
  DW_AT_const_value = 0x1c,
  DW_AT_declaration = 0x3c,
  DW_AT_stmt_list = 0x10,
  DW_AT_low_pc = 0x11,
  DW_AT_high_pc = 0x12,
  DW_AT_language = 0x13,
  DW_AT_comp_dir = 0x1b,
  DW_AT_producer = 0x25,
  DW_AT_external = 0x3f,
  DW_FORM_addr = 0x01,
  DW_FORM_data1 = 0x0b,
  DW_FORM_data2 = 0x05,
  DW_FORM_data4 = 0x06,
  DW_FORM_data8 = 0x07,
  DW_FORM_sdata = 0x0d,
  DW_FORM_udata = 0x0f,
  DW_FORM_string = 0x08,
  DW_FORM_ref4 = 0x13,
  DW_FORM_flag = 0x0c,
  DW_FORM_flag_present = 0x19,
  DW_FORM_exprloc = 0x18,
  DW_FORM_sec_offset = 0x17,
  DW_ATE_boolean = 0x02,
  DW_ATE_float = 0x04,
  DW_ATE_signed = 0x05,
  DW_ATE_signed_char = 0x06,
  DW_ATE_unsigned = 0x07,
  DW_ATE_unsigned_char = 0x08,
  DW_OP_deref = 0x06,
  DW_OP_fbreg = 0x91,
  DW_OP_reg6 = 0x56,  // x86_64 rbp
  DW_OP_reg29 = 0x6d, // aarch64 x29
  DW_LANG_C99 = 0x0c,
  DW_LNS_copy = 1,
  DW_LNS_advance_pc = 2,
  DW_LNS_advance_line = 3,
  DW_LNS_set_file = 4,
  DW_LNS_set_prologue_end = 10,
  DW_LNE_end_sequence = 1,
  DW_LNE_set_address = 2,
};

// ---- Persistent debug-type IR (interned from slimcc Types) ----------------
// A self-contained type graph snapshotted while the slimcc Type is still valid
// (its arena is recycled per compile), accumulated across a debug build and
// cleared by slimcc_debug_reset. The DWARF emitter walks this, not slimcc's
// internals. Type index 0 is always "void".
enum {
  DT_VOID, DT_BASE, DT_PTR, DT_ARRAY, DT_STRUCT, DT_UNION,
  DT_ENUM, DT_TYPEDEF, DT_FUNC
};
typedef struct {
  int kind;
  int enc;       // DT_BASE: DW_ATE_*
  int64_t size;  // byte size (0 = unknown/void)
  int ref;       // referenced type (ptr-to / array-elem / func-return), or -1
  char *name;    // type name, or NULL (anonymous)
  int64_t count; // DT_ARRAY: element count, or -1 if unknown
  int memb, nmemb; // DT_STRUCT/UNION/ENUM/FUNC: contiguous slice of dbg_members
} DbgType;
typedef struct {
  char *name;   // member / enumerator / param name, or NULL
  int type;     // member / param type index; -1 for an enumerator
  int64_t off;  // member byte offset; or enumerator const value
  int bit_size; // bitfield width (0 = not a bitfield)
  int bit_off;  // bitfield's absolute bit offset from the object start
} DbgMember;

static struct { DbgType *v; int len, cap; } dbg_types;
static struct { DbgMember *v; int len, cap; } dbg_members;
typedef struct { char *func, *name; int type, is_param; unsigned reg; } DbgLocal;
static struct { DbgLocal *v; int len, cap; } dbg_locals;
// Per-add_local cycle map (Type* -> type index): recursive structs reference
// themselves, so a node is registered before its members are interned.
static struct { Type **k; int *v; int len, cap; } dbg_visit;

static char *dbg_strndup(const char *s, int n) {
  char *r = malloc(n + 1);
  memcpy(r, s, n);
  r[n] = 0;
  return r;
}
static int dbg_new_type(void) {
  if (dbg_types.len == dbg_types.cap) {
    dbg_types.cap = dbg_types.cap ? dbg_types.cap * 2 : 32;
    dbg_types.v = realloc(dbg_types.v, dbg_types.cap * sizeof(DbgType));
  }
  int i = dbg_types.len++;
  dbg_types.v[i] = (DbgType){.kind = DT_VOID, .ref = -1, .count = -1, .memb = -1};
  return i;
}
static void dbg_visit_add(Type *ty, int idx) {
  if (dbg_visit.len == dbg_visit.cap) {
    dbg_visit.cap = dbg_visit.cap ? dbg_visit.cap * 2 : 16;
    dbg_visit.k = realloc(dbg_visit.k, dbg_visit.cap * sizeof(Type *));
    dbg_visit.v = realloc(dbg_visit.v, dbg_visit.cap * sizeof(int));
  }
  dbg_visit.k[dbg_visit.len] = ty;
  dbg_visit.v[dbg_visit.len] = idx;
  dbg_visit.len++;
}
static int dbg_push_member(char *name, int type, int64_t off, int bsz, int boff) {
  if (dbg_members.len == dbg_members.cap) {
    dbg_members.cap = dbg_members.cap ? dbg_members.cap * 2 : 64;
    dbg_members.v = realloc(dbg_members.v, dbg_members.cap * sizeof(DbgMember));
  }
  int i = dbg_members.len++;
  dbg_members.v[i] = (DbgMember){.name = name, .type = type, .off = off, .bit_size = bsz, .bit_off = boff};
  return i;
}

static int intern_type(Type *ty);

// A base (scalar) type: pick a DWARF encoding + canonical name from the kind.
static int intern_base(Type *ty, int enc, const char *name) {
  int i = dbg_new_type();
  dbg_visit_add(ty, i);
  DbgType *t = &dbg_types.v[i];
  t->kind = DT_BASE;
  t->enc = enc;
  t->size = ty->size;
  t->name = strdup(name);
  return i;
}

static int intern_type(Type *ty) {
  if (ty == NULL || ty->kind == TY_VOID) return 0; // 0 == void
  int seen;
  for (seen = 0; seen < dbg_visit.len; seen++)
    if (dbg_visit.k[seen] == ty) return dbg_visit.v[seen];

  // Enum variables carry their underlying integer kind but keep the enumerator
  // list; emit a real enumeration_type so a debugger shows enumerator names.
  if (ty->enums != NULL && ty->kind != TY_ENUM) {
    int i = dbg_new_type();
    dbg_visit_add(ty, i);
    int start = dbg_members.len, n = 0;
    for (EnumVal *e = ty->enums; e; e = e->next) {
      dbg_push_member(e->name ? dbg_strndup(e->name->loc, e->name->len) : NULL, -1, e->val, 0, 0);
      n++;
    }
    DbgType *t = &dbg_types.v[i];
    t->kind = DT_ENUM;
    t->size = ty->size;
    t->name = ty->tag ? dbg_strndup(ty->tag->loc, ty->tag->len) : NULL;
    t->memb = start;
    t->nmemb = n;
    return i;
  }

  switch (ty->kind) {
  case TY_BOOL: return intern_base(ty, DW_ATE_boolean, "_Bool");
  case TY_FLOAT: return intern_base(ty, DW_ATE_float, "float");
  case TY_DOUBLE: return intern_base(ty, DW_ATE_float, "double");
  case TY_LDOUBLE: return intern_base(ty, DW_ATE_float, "long double");
  case TY_PCHAR: case TY_CHAR:
    return intern_base(ty, ty->is_unsigned ? DW_ATE_unsigned_char : DW_ATE_signed_char,
                       ty->is_unsigned ? "unsigned char" : "char");
  case TY_SHORT:
    return intern_base(ty, ty->is_unsigned ? DW_ATE_unsigned : DW_ATE_signed,
                       ty->is_unsigned ? "unsigned short" : "short");
  case TY_INT:
    return intern_base(ty, ty->is_unsigned ? DW_ATE_unsigned : DW_ATE_signed,
                       ty->is_unsigned ? "unsigned int" : "int");
  case TY_LONG:
    return intern_base(ty, ty->is_unsigned ? DW_ATE_unsigned : DW_ATE_signed,
                       ty->is_unsigned ? "unsigned long" : "long");
  case TY_LONGLONG:
    return intern_base(ty, ty->is_unsigned ? DW_ATE_unsigned : DW_ATE_signed,
                       ty->is_unsigned ? "unsigned long long" : "long long");
  case TY_BITINT:
    return intern_base(ty, ty->is_unsigned ? DW_ATE_unsigned : DW_ATE_signed, "_BitInt");
  case TY_PTR: case TY_NULLPTR: {
    int i = dbg_new_type();
    dbg_visit_add(ty, i);
    int r = intern_type(ty->base); // may grow dbg_types -> re-index below
    dbg_types.v[i].kind = DT_PTR;
    dbg_types.v[i].size = ty->size;
    dbg_types.v[i].ref = r;
    return i;
  }
  case TY_ARRAY: {
    int i = dbg_new_type();
    dbg_visit_add(ty, i);
    int r = intern_type(ty->base);
    dbg_types.v[i].kind = DT_ARRAY;
    dbg_types.v[i].size = ty->size;
    dbg_types.v[i].ref = r;
    dbg_types.v[i].count = ty->array_len;
    return i;
  }
  case TY_STRUCT: case TY_UNION: {
    int i = dbg_new_type();
    dbg_visit_add(ty, i);
    // Intern member types first (may add more types/members), collecting their
    // indices, then append this aggregate's members contiguously.
    int n = 0;
    for (Member *m = ty->members; m; m = m->next) n++;
    int *mtypes = n ? malloc(n * sizeof(int)) : NULL;
    int k = 0;
    for (Member *m = ty->members; m; m = m->next) mtypes[k++] = intern_type(m->ty);
    int start = dbg_members.len;
    k = 0;
    for (Member *m = ty->members; m; m = m->next, k++) {
      char *nm = m->name ? dbg_strndup(m->name->loc, m->name->len) : NULL;
      int bsz = m->is_bitfield ? m->bit_width : 0;
      int boff = m->is_bitfield ? (int)(m->offset * 8 + m->bit_offset) : 0;
      dbg_push_member(nm, mtypes[k], m->offset, bsz, boff);
    }
    free(mtypes);
    DbgType *t = &dbg_types.v[i];
    t->kind = ty->kind == TY_UNION ? DT_UNION : DT_STRUCT;
    t->size = ty->size;
    t->name = ty->tag ? dbg_strndup(ty->tag->loc, ty->tag->len) : NULL;
    t->memb = start;
    t->nmemb = n;
    return i;
  }
  case TY_ENUM: {
    int i = dbg_new_type();
    dbg_visit_add(ty, i);
    int start = dbg_members.len, n = 0;
    for (EnumVal *e = ty->enums; e; e = e->next) {
      char *nm = e->name ? dbg_strndup(e->name->loc, e->name->len) : NULL;
      dbg_push_member(nm, -1, e->val, 0, 0);
      n++;
    }
    DbgType *t = &dbg_types.v[i];
    t->kind = DT_ENUM;
    t->size = ty->size;
    t->name = ty->tag ? dbg_strndup(ty->tag->loc, ty->tag->len) : NULL;
    t->memb = start;
    t->nmemb = n;
    return i;
  }
  case TY_FUNC: {
    int i = dbg_new_type();
    dbg_visit_add(ty, i);
    int rr = intern_type(ty->return_ty);
    int n = 0;
    for (Obj *p = ty->param_list; p; p = p->param_next) n++;
    int *ptypes = n ? malloc(n * sizeof(int)) : NULL;
    int k = 0;
    for (Obj *p = ty->param_list; p; p = p->param_next) ptypes[k++] = intern_type(p->ty);
    int start = dbg_members.len;
    for (k = 0; k < n; k++) dbg_push_member(NULL, ptypes[k], 0, 0, 0);
    free(ptypes);
    DbgType *t = &dbg_types.v[i];
    t->kind = DT_FUNC;
    t->ref = rr;
    t->memb = start;
    t->nmemb = n;
    return i;
  }
  default: // VLA, _BitInt>handled, auto, asm, ... -> describe as void
    return 0;
  }
}

void slimcc_debug_add_local(const char *func, const char *name, Type *ty,
                            int is_param, unsigned reg) {
  for (int i = 0; i < dbg_locals.len; i++) // dedup param-vs-scope double listing
    if (dbg_locals.v[i].reg == reg && strcmp(dbg_locals.v[i].func, func) == 0) return;
  if (dbg_types.len == 0) { // seed index 0 = void
    int v = dbg_new_type();
    dbg_types.v[v].kind = DT_VOID;
    dbg_types.v[v].name = strdup("void");
  }
  dbg_visit.len = 0;
  int type = intern_type(ty);
  if (dbg_locals.len == dbg_locals.cap) {
    dbg_locals.cap = dbg_locals.cap ? dbg_locals.cap * 2 : 16;
    dbg_locals.v = realloc(dbg_locals.v, dbg_locals.cap * sizeof(DbgLocal));
  }
  dbg_locals.v[dbg_locals.len++] =
    (DbgLocal){.func = strdup(func), .name = strdup(name), .type = type,
               .is_param = is_param, .reg = reg};
}

void slimcc_debug_reset(void) {
  for (int i = 0; i < dbg_files.len; i++) free(dbg_files.names[i]);
  free(dbg_files.names);
  dbg_files.names = NULL;
  dbg_files.len = dbg_files.cap = 0;
  for (int i = 0; i < dbg_types.len; i++) free(dbg_types.v[i].name);
  free(dbg_types.v);
  dbg_types.v = NULL;
  dbg_types.len = dbg_types.cap = 0;
  for (int i = 0; i < dbg_members.len; i++) free(dbg_members.v[i].name);
  free(dbg_members.v);
  dbg_members.v = NULL;
  dbg_members.len = dbg_members.cap = 0;
  for (int i = 0; i < dbg_locals.len; i++) {
    free(dbg_locals.v[i].func);
    free(dbg_locals.v[i].name);
  }
  free(dbg_locals.v);
  dbg_locals.v = NULL;
  dbg_locals.len = dbg_locals.cap = 0;
  free(dbg_visit.k);
  free(dbg_visit.v);
  dbg_visit.k = NULL;
  dbg_visit.v = NULL;
  dbg_visit.len = dbg_visit.cap = 0;
}

// .debug_abbrev: 1=compile_unit, 2=subprogram, 3=base_type, 4=pointer_type,
// 5=variable, 6=formal_parameter.
// Abbrev codes (must match what dwarf_info emits).
enum {
  A_CU = 1, A_SUBPROG, A_BASE, A_PTR, A_VAR, A_PARAM, A_VOIDT,
  A_STRUCT, A_UNION, A_MEMBER, A_MEMBERBF, A_ARRAY, A_SUBRANGE, A_SUBRANGE0,
  A_ENUM, A_ENUMERATOR, A_SUBR, A_FPARAM,
};

static void abbrev_attr(Buf *b, int at, int form) { buf_uleb(b, at); buf_uleb(b, form); }
static void abbrev_hdr(Buf *b, int code, int tag, int children) {
  buf_uleb(b, code); buf_uleb(b, tag); buf_u8(b, children);
}

static void dwarf_abbrev(Buf *b) {
  abbrev_hdr(b, A_CU, DW_TAG_compile_unit, DW_CHILDREN_yes);
  abbrev_attr(b, DW_AT_producer, DW_FORM_string);
  abbrev_attr(b, DW_AT_language, DW_FORM_data2);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_comp_dir, DW_FORM_string);
  abbrev_attr(b, DW_AT_low_pc, DW_FORM_addr);
  abbrev_attr(b, DW_AT_high_pc, DW_FORM_addr);
  abbrev_attr(b, DW_AT_stmt_list, DW_FORM_sec_offset);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_SUBPROG, DW_TAG_subprogram, DW_CHILDREN_yes);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_low_pc, DW_FORM_addr);
  abbrev_attr(b, DW_AT_high_pc, DW_FORM_addr);
  abbrev_attr(b, DW_AT_frame_base, DW_FORM_exprloc);
  abbrev_attr(b, DW_AT_external, DW_FORM_flag);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_BASE, DW_TAG_base_type, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_encoding, DW_FORM_data1);
  abbrev_attr(b, DW_AT_byte_size, DW_FORM_data1);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_PTR, DW_TAG_pointer_type, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_byte_size, DW_FORM_data1);
  abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
  buf_uleb(b, 0); buf_uleb(b, 0);

  for (int v = 0; v < 2; v++) { // A_VAR, A_PARAM
    abbrev_hdr(b, v ? A_PARAM : A_VAR, v ? DW_TAG_formal_parameter : DW_TAG_variable,
               DW_CHILDREN_no);
    abbrev_attr(b, DW_AT_name, DW_FORM_string);
    abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
    abbrev_attr(b, DW_AT_location, DW_FORM_exprloc);
    buf_uleb(b, 0); buf_uleb(b, 0);
  }

  abbrev_hdr(b, A_VOIDT, DW_TAG_unspecified_type, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  buf_uleb(b, 0); buf_uleb(b, 0);

  for (int u = 0; u < 2; u++) { // A_STRUCT, A_UNION
    abbrev_hdr(b, u ? A_UNION : A_STRUCT, u ? DW_TAG_union_type : DW_TAG_structure_type,
               DW_CHILDREN_yes);
    abbrev_attr(b, DW_AT_name, DW_FORM_string);
    abbrev_attr(b, DW_AT_byte_size, DW_FORM_udata);
    buf_uleb(b, 0); buf_uleb(b, 0);
  }

  abbrev_hdr(b, A_MEMBER, DW_TAG_member, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
  abbrev_attr(b, DW_AT_data_member_location, DW_FORM_udata);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_MEMBERBF, DW_TAG_member, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
  abbrev_attr(b, DW_AT_data_bit_offset, DW_FORM_udata);
  abbrev_attr(b, DW_AT_bit_size, DW_FORM_udata);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_ARRAY, DW_TAG_array_type, DW_CHILDREN_yes);
  abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_SUBRANGE, DW_TAG_subrange_type, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_count, DW_FORM_udata);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_SUBRANGE0, DW_TAG_subrange_type, DW_CHILDREN_no);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_ENUM, DW_TAG_enumeration_type, DW_CHILDREN_yes);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_byte_size, DW_FORM_udata);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_ENUMERATOR, DW_TAG_enumerator, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_name, DW_FORM_string);
  abbrev_attr(b, DW_AT_const_value, DW_FORM_sdata);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_SUBR, DW_TAG_subroutine_type, DW_CHILDREN_yes);
  abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
  buf_uleb(b, 0); buf_uleb(b, 0);

  abbrev_hdr(b, A_FPARAM, DW_TAG_formal_parameter, DW_CHILDREN_no);
  abbrev_attr(b, DW_AT_type, DW_FORM_ref4);
  buf_uleb(b, 0); buf_uleb(b, 0);

  buf_uleb(b, 0); // end of table
}

// .debug_info: a CU holding the interned type graph (base/pointer/struct/union/
// array/enum/function types) followed by a subprogram DIE per function, each
// with a child variable/parameter DIE (located relative to the frame pointer)
// for every local the compiler recorded. Inter-type ref4s are backpatched after
// all type DIEs are laid out, since recursive types reference forward.
typedef struct { size_t pos; int target; } TFix;

static void dwarf_info(Buf *b, const slimcc_jitsym *syms, int nsyms,
                       uint64_t text_base, uint64_t text_size, const char *cu_name) {
  size_t unit_len_pos = b->len;
  buf_u32(b, 0); // unit_length, patched below
  size_t after_len = b->len;
  buf_u16(b, 4);  // DWARF version
  buf_u32(b, 0);  // .debug_abbrev offset
  buf_u8(b, 8);   // address size
  buf_uleb(b, A_CU);
  buf_str(b, "slimcc");
  buf_u16(b, DW_LANG_C99);
  buf_str(b, cu_name ? cu_name : "cdef");
  buf_str(b, "");
  buf_u64(b, text_base);
  buf_u64(b, text_base + text_size);
  buf_u32(b, 0); // stmt_list -> .debug_line offset 0

  uint32_t *toff = dbg_types.len ? calloc(dbg_types.len, sizeof(uint32_t)) : NULL;
  TFix *fix = NULL;
  int nfix = 0, cfix = 0;
#define REF(target_)                                                                \
  do {                                                                              \
    if (nfix == cfix) { cfix = cfix ? cfix * 2 : 64; fix = realloc(fix, cfix * sizeof(TFix)); } \
    fix[nfix].pos = b->len; fix[nfix].target = (target_) < 0 ? 0 : (target_); nfix++; \
    buf_u32(b, 0);                                                                   \
  } while (0)

  for (int t = 0; t < dbg_types.len; t++) {
    DbgType *dt = &dbg_types.v[t];
    toff[t] = (uint32_t)(b->len - unit_len_pos);
    switch (dt->kind) {
    case DT_BASE:
      buf_uleb(b, A_BASE); buf_str(b, dt->name ? dt->name : "");
      buf_u8(b, (uint8_t)dt->enc); buf_u8(b, (uint8_t)dt->size);
      break;
    case DT_PTR:
      buf_uleb(b, A_PTR); buf_u8(b, (uint8_t)(dt->size ? dt->size : 8)); REF(dt->ref);
      break;
    case DT_STRUCT: case DT_UNION:
      buf_uleb(b, dt->kind == DT_UNION ? A_UNION : A_STRUCT);
      buf_str(b, dt->name ? dt->name : "");
      buf_uleb(b, (uint64_t)dt->size);
      for (int m = dt->memb; m < dt->memb + dt->nmemb; m++) {
        DbgMember *dm = &dbg_members.v[m];
        if (dm->bit_size > 0) {
          buf_uleb(b, A_MEMBERBF); buf_str(b, dm->name ? dm->name : ""); REF(dm->type);
          buf_uleb(b, (uint64_t)dm->bit_off); buf_uleb(b, (uint64_t)dm->bit_size);
        } else {
          buf_uleb(b, A_MEMBER); buf_str(b, dm->name ? dm->name : ""); REF(dm->type);
          buf_uleb(b, (uint64_t)dm->off);
        }
      }
      buf_u8(b, 0);
      break;
    case DT_ARRAY:
      buf_uleb(b, A_ARRAY); REF(dt->ref);
      if (dt->count >= 0) { buf_uleb(b, A_SUBRANGE); buf_uleb(b, (uint64_t)dt->count); }
      else buf_uleb(b, A_SUBRANGE0);
      buf_u8(b, 0);
      break;
    case DT_ENUM:
      buf_uleb(b, A_ENUM); buf_str(b, dt->name ? dt->name : ""); buf_uleb(b, (uint64_t)dt->size);
      for (int m = dt->memb; m < dt->memb + dt->nmemb; m++) {
        DbgMember *dm = &dbg_members.v[m];
        buf_uleb(b, A_ENUMERATOR); buf_str(b, dm->name ? dm->name : ""); buf_sleb(b, dm->off);
      }
      buf_u8(b, 0);
      break;
    case DT_FUNC:
      buf_uleb(b, A_SUBR); REF(dt->ref);
      for (int m = dt->memb; m < dt->memb + dt->nmemb; m++) {
        buf_uleb(b, A_FPARAM); REF(dbg_members.v[m].type);
      }
      buf_u8(b, 0);
      break;
    default: // DT_VOID and anything unmodeled
      buf_uleb(b, A_VOIDT); buf_str(b, dt->name ? dt->name : "void");
      break;
    }
  }
  for (int f = 0; f < nfix; f++) memcpy(b->p + fix[f].pos, &toff[fix[f].target], 4);
  free(fix);

  for (int i = 0; i < nsyms; i++) {
    if (!syms[i].is_func || !syms[i].addr) continue;
    buf_uleb(b, A_SUBPROG);
    buf_str(b, syms[i].name ? syms[i].name : "");
    buf_u64(b, (uint64_t)(uintptr_t)syms[i].addr);
    buf_u64(b, (uint64_t)(uintptr_t)syms[i].addr + syms[i].size);
    buf_u8(b, 1); buf_u8(b, SLIMCC_DW_OP_FP); // frame_base = exprloc{DW_OP_regFP}
    buf_u8(b, 1); // external
    MIR_func_t fn = (MIR_func_t)syms[i].mir_func;
    if (fn != NULL) {
      for (int k = 0; k < dbg_locals.len; k++) {
        DbgLocal *d = &dbg_locals.v[k];
        if (strcmp(d->func, syms[i].name ? syms[i].name : "") != 0) continue;
        int64_t off;
        if (!MIR_reg_frame_offset(fn, d->reg, &off)) continue; // not stack-homed
        buf_uleb(b, d->is_param ? A_PARAM : A_VAR);
        buf_str(b, d->name);
        buf_u32(b, toff ? toff[d->type] : 0); // all type offsets known now
        // location = DW_OP_fbreg(off), DW_OP_deref: the slot holds the variable's
        // address (the alloca pointer), so dereference to reach the variable.
        Buf e = {0};
        buf_u8(&e, DW_OP_fbreg); buf_sleb(&e, off); buf_u8(&e, DW_OP_deref);
        buf_uleb(b, e.len);
        buf_bytes(b, e.p, e.len);
        free(e.p);
      }
    }
    buf_u8(b, 0); // end subprogram children
  }
  buf_u8(b, 0); // end of CU children
  free(toff);
  uint32_t unit_len = (uint32_t)(b->len - after_len);
  memcpy(b->p + unit_len_pos, &unit_len, 4);
#undef REF
}

// .debug_line: a DWARF4 line program with one sequence per function. file ids
// in the line maps are 1-based indices into the debug file table (dbg_files).
static void dwarf_line(Buf *b, const slimcc_jitsym *syms, int nsyms) {
  size_t unit_len_pos = b->len;
  buf_u32(b, 0); // unit_length, patched
  size_t after_len = b->len;
  buf_u16(b, 4); // version
  size_t hdr_len_pos = b->len;
  buf_u32(b, 0); // header_length, patched
  size_t after_hdr_len = b->len;
  buf_u8(b, 1);  // minimum_instruction_length
  buf_u8(b, 1);  // maximum_operations_per_instruction
  buf_u8(b, 1);  // default_is_stmt
  buf_u8(b, (uint8_t)(int8_t)-5); // line_base
  buf_u8(b, 14); // line_range
  buf_u8(b, 13); // opcode_base
  static const uint8_t std_lens[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
  buf_bytes(b, std_lens, sizeof std_lens);
  buf_u8(b, 0); // include_directories: empty list terminator
  for (int i = 0; i < dbg_files.len; i++) { // file_names (1-based)
    buf_str(b, dbg_files.names[i]);
    buf_uleb(b, 0); // dir index
    buf_uleb(b, 0); // mtime
    buf_uleb(b, 0); // size
  }
  buf_u8(b, 0); // file_names terminator
  uint32_t hdr_len = (uint32_t)(b->len - after_hdr_len);
  memcpy(b->p + hdr_len_pos, &hdr_len, 4);

  for (int i = 0; i < nsyms; i++) {
    if (!syms[i].is_func || !syms[i].addr || !syms[i].line_map || syms[i].line_map_len == 0)
      continue;
    // set_address to the function's runtime address
    buf_u8(b, 0); buf_uleb(b, 9); buf_u8(b, DW_LNE_set_address);
    buf_u64(b, (uint64_t)(uintptr_t)syms[i].addr);
    uint32_t cur_off = 0, cur_line = 1, cur_file = 1;
    int prologue_marked = 0;
    // The generated prologue is left unattributed, so the first real entry is
    // at offset > 0. Emit a row at the entry point (low_pc) carrying that first
    // line, so a debugger has a line for the function's address and `break
    // func` resolves; the matching entry below then carries prologue_end.
    {
      const MIR_line_map_t *e0 = &syms[i].line_map[0];
      uint32_t f0 = e0->file_id ? e0->file_id : 1;
      if (f0 != cur_file) { buf_u8(b, DW_LNS_set_file); buf_uleb(b, f0); cur_file = f0; }
      buf_u8(b, DW_LNS_advance_line); buf_sleb(b, (int64_t)e0->line - (int64_t)cur_line);
      cur_line = e0->line;
      buf_u8(b, DW_LNS_copy); // row at offset 0
    }
    for (size_t j = 0; j < syms[i].line_map_len; j++) {
      const MIR_line_map_t *e = &syms[i].line_map[j];
      uint32_t file = e->file_id ? e->file_id : 1;
      if (file != cur_file) { buf_u8(b, DW_LNS_set_file); buf_uleb(b, file); cur_file = file; }
      if (e->line != cur_line) {
        buf_u8(b, DW_LNS_advance_line);
        buf_sleb(b, (int64_t)e->line - (int64_t)cur_line);
        cur_line = e->line;
      }
      if (e->code_offset != cur_off) {
        buf_u8(b, DW_LNS_advance_pc);
        buf_uleb(b, (uint64_t)(e->code_offset - cur_off));
        cur_off = e->code_offset;
      }
      // Mark the first row past the entry point as prologue end, so gdb's
      // `break func` lands on the first real statement rather than the
      // (imperfectly attributed) entry row.
      if (!prologue_marked && e->code_offset != 0) {
        buf_u8(b, DW_LNS_set_prologue_end);
        prologue_marked = 1;
      }
      buf_u8(b, DW_LNS_copy);
    }
    // advance to function end and close the sequence
    if (syms[i].size > cur_off) {
      buf_u8(b, DW_LNS_advance_pc);
      buf_uleb(b, (uint64_t)(syms[i].size - cur_off));
    }
    buf_u8(b, 0); buf_uleb(b, 1); buf_u8(b, DW_LNE_end_sequence);
  }
  uint32_t unit_len = (uint32_t)(b->len - after_len);
  memcpy(b->p + unit_len_pos, &unit_len, 4);
}

int slimcc_debug_obj(const slimcc_jitsym *syms, int nsyms, void **buf,
                     size_t *size, char **errmsg) {
  if (errmsg) *errmsg = NULL;
  if (buf) *buf = NULL;
  if (size) *size = 0;
  if (nsyms < 0 || (nsyms > 0 && syms == NULL) || buf == NULL || size == NULL)
    return debug_obj_fail(errmsg, "slimcc_debug_obj: invalid arguments");
  if (SLIMCC_ELF_MACHINE == EM_NONE)
    return debug_obj_fail(errmsg, "slimcc_debug_obj: unsupported host architecture");

  // gdb's JIT reader only materializes symbols that fall inside an allocatable
  // section of the registered object (a symtab-only/SHN_ABS object yields no
  // usable symbols). So anchor every symbol to a single SHT_NOBITS .text
  // section spanning their address range, and define each symbol relative to
  // it (st_shndx = .text, st_value = addr - text_base). gdb places the section
  // at sh_addr and reads the actual instruction bytes from inferior memory.
  uintptr_t lo = UINTPTR_MAX, hi = 0;
  int have_lines = 0;
  for (int i = 0; i < nsyms; i++) {
    if (!syms[i].addr) continue;
    uintptr_t a = (uintptr_t)syms[i].addr;
    uintptr_t e = a + (syms[i].size ? syms[i].size : 1);
    if (a < lo) lo = a;
    if (e > hi) hi = e;
    if (syms[i].is_func && syms[i].line_map && syms[i].line_map_len) have_lines = 1;
  }
  if (lo == UINTPTR_MAX) lo = hi = 0;
  uint64_t text_base = lo, text_size = hi > lo ? (uint64_t)(hi - lo) : 0;

  // Build the section bodies. DWARF debug sections only when line maps exist.
  Buf strtab = {0}, symtab = {0}, abbrev = {0}, info = {0}, line = {0};
  buf_u8(&strtab, 0); // leading NUL
  // null symbol
  Elf64_Sym z = {0};
  buf_bytes(&symtab, &z, sizeof z);
  for (int i = 0; i < nsyms; i++) {
    const char *nm = syms[i].name ? syms[i].name : "";
    Elf64_Sym s = {0};
    s.st_name = (Elf64_Word)strtab.len;
    buf_str(&strtab, nm);
    s.st_info = ELF64_ST_INFO(STB_GLOBAL, syms[i].is_func ? STT_FUNC : STT_OBJECT);
    s.st_other = STV_DEFAULT;
    if (syms[i].addr) {
      s.st_shndx = 1; // .text
      s.st_value = (Elf64_Addr)((uintptr_t)syms[i].addr - text_base);
    } else {
      s.st_shndx = SHN_UNDEF;
    }
    s.st_size = (Elf64_Xword)syms[i].size;
    buf_bytes(&symtab, &s, sizeof s);
  }
  const char *cu_name = dbg_files.len ? dbg_files.names[0] : "cdef";
  if (have_lines) {
    dwarf_abbrev(&abbrev);
    dwarf_info(&info, syms, nsyms, text_base, text_size, cu_name);
    dwarf_line(&line, syms, nsyms);
  }

  // Section table. Indices must match the symtab's st_shndx (.text = 1) and the
  // symtab's sh_link (.strtab). Order: null, .text, .symtab, .strtab, [debug], .shstrtab.
  struct {
    const char *name;
    uint32_t type, link, info, align;
    uint64_t flags, addr, entsize;
    const Buf *body; // NULL => NOBITS sized by `size`
    uint64_t size;
  } S[16];
  int ns = 0;
  S[ns++] = (typeof(S[0])){0};
  int i_text = ns;
  S[ns++] = (typeof(S[0])){".text", SHT_NOBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR,
                           text_base, 0, NULL, text_size};
  int i_strtab_pending = -1; // fixed after we know .strtab index
  int i_symtab = ns;
  S[ns++] = (typeof(S[0])){".symtab", SHT_SYMTAB, 0 /*link set below*/, 1, 8, 0, 0,
                           sizeof(Elf64_Sym), &symtab, symtab.len};
  int i_strtab = ns;
  S[ns++] = (typeof(S[0])){".strtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &strtab, strtab.len};
  S[i_symtab].link = (uint32_t)i_strtab;
  (void)i_strtab_pending;
  if (have_lines) {
    S[ns++] = (typeof(S[0])){".debug_abbrev", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &abbrev, abbrev.len};
    S[ns++] = (typeof(S[0])){".debug_info", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &info, info.len};
    S[ns++] = (typeof(S[0])){".debug_line", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &line, line.len};
  }
  int i_shstr = ns;
  // .shstrtab body built from the section names.
  Buf shstr = {0};
  buf_u8(&shstr, 0);
  uint32_t name_off[16];
  name_off[0] = 0;
  for (int i = 1; i < ns; i++) { name_off[i] = (uint32_t)shstr.len; buf_str(&shstr, S[i].name); }
  uint32_t name_shstr = (uint32_t)shstr.len;
  buf_str(&shstr, ".shstrtab");
  S[ns++] = (typeof(S[0])){".shstrtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &shstr, shstr.len};
  name_off[i_shstr] = name_shstr;
  (void)i_text;

  // Lay out: ehdr, then each section body (8-aligned), then section headers.
  size_t off = sizeof(Elf64_Ehdr);
  uint64_t sec_off[16] = {0};
  for (int i = 1; i < ns; i++) {
    if (S[i].type == SHT_NOBITS) { sec_off[i] = off; continue; }
    off = (off + 7) & ~(size_t)7;
    sec_off[i] = off;
    off += S[i].size;
  }
  off = (off + 7) & ~(size_t)7;
  size_t shoff = off;
  size_t total = shoff + (size_t)ns * sizeof(Elf64_Shdr);

  unsigned char *p = calloc(1, total);
  if (!p) {
    free(strtab.p); free(symtab.p); free(abbrev.p); free(info.p); free(line.p); free(shstr.p);
    return debug_obj_fail(errmsg, "slimcc_debug_obj: out of memory");
  }

  Elf64_Ehdr *eh = (Elf64_Ehdr *)p;
  eh->e_ident[EI_MAG0] = ELFMAG0;
  eh->e_ident[EI_MAG1] = ELFMAG1;
  eh->e_ident[EI_MAG2] = ELFMAG2;
  eh->e_ident[EI_MAG3] = ELFMAG3;
  eh->e_ident[EI_CLASS] = ELFCLASS64;
  eh->e_ident[EI_DATA] = ELFDATA2LSB;
  eh->e_ident[EI_VERSION] = EV_CURRENT;
  eh->e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh->e_type = ET_REL;
  eh->e_machine = SLIMCC_ELF_MACHINE;
  eh->e_version = EV_CURRENT;
  eh->e_shoff = shoff;
  eh->e_ehsize = sizeof(Elf64_Ehdr);
  eh->e_shentsize = sizeof(Elf64_Shdr);
  eh->e_shnum = (Elf64_Half)ns;
  eh->e_shstrndx = (Elf64_Half)i_shstr;

  Elf64_Shdr *sh = (Elf64_Shdr *)(p + shoff);
  for (int i = 1; i < ns; i++) {
    if (S[i].type != SHT_NOBITS && S[i].body && S[i].size)
      memcpy(p + sec_off[i], S[i].body->p, S[i].size);
    sh[i].sh_name = name_off[i];
    sh[i].sh_type = S[i].type;
    sh[i].sh_flags = S[i].flags;
    sh[i].sh_addr = S[i].addr;
    sh[i].sh_offset = (S[i].type == SHT_NOBITS) ? 0 : sec_off[i];
    sh[i].sh_size = S[i].size;
    sh[i].sh_link = S[i].link;
    sh[i].sh_info = S[i].info;
    sh[i].sh_addralign = S[i].align;
    sh[i].sh_entsize = S[i].entsize;
  }

  free(strtab.p); free(symtab.p); free(abbrev.p); free(info.p); free(line.p); free(shstr.p);
  *buf = p;
  *size = total;
  return 0;
}
