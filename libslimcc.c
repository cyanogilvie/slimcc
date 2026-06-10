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

void add_dep_file(const char *path, bool is_sys) {
  (void)path, (void)is_sys;
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
  include_paths = (StringArray){0};
  iquote_paths = (StringArray){0};
  display_files = (StringArray){0};
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

  reset_all();

  diag_buf = NULL;
  slimcc_diag_file = open_memstream(&diag_buf, &diag_len);

  // The whole pipeline, MIR build errors included, unwinds to here.
  // The scratch context is deliberately leaked on failure: it may hold a
  // half-built function/module that MIR_finish would refuse to clean up.
  MIR_context_t scratch = MIR_init();
  MIR_set_error_func(scratch, mir_error);

  if (setjmp(compile_jmp)) {
    compile_active = false;
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
  compile_active = true;

  slimcc_vfile_add(name, source);
  if (opt) {
    for (int i = 0; i < opt->n_vfiles; i++)
      slimcc_vfile_add(opt->vfiles[i].name, opt->vfiles[i].contents);
    for (int i = 0; i < opt->n_include_paths; i++)
      add_include_path(&include_paths, opt->include_paths[i]);
  }

  arena_on(&cc1_arena);
  arena_on(&pp_arena);

  codegen_mir_begin(scratch, name);

  init_macros();
  platform_init_cc1();
  lib_macros();
  if (opt)
    for (int i = 0; i < opt->n_defines; i++)
      define_macro_cli(opt->defines[i]);

  Token *tok = preprocess(name, &(StringArray){0}, &(StringArray){0});
  tok = prepare_parse(tok);
  arena_off(&pp_arena);

  Obj *prog = parse(tok);
  codegen(prog, NULL);

  arena_off(&cc1_arena);
  compile_active = false;

  MIR_module_t mod = codegen_mir_result();
  if (opt && opt->mir_dump)
    MIR_output_module(scratch, opt->mir_dump, mod);
  MIR_change_module_ctx(scratch, mod, ctx);
  MIR_finish(scratch);

  fclose(slimcc_diag_file);
  slimcc_diag_file = NULL;
  free(diag_buf); // warnings only; surfaced via a callback in a later version
  diag_buf = NULL;
  return mod;
}

void slimcc_register_helpers(MIR_context_t ctx) {
  MIR_load_external(ctx, "memset", (void *)memset);
  MIR_load_external(ctx, "memcpy", (void *)memcpy);
}
