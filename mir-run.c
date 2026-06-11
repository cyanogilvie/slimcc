// slimcc-mir-run: test driver for the MIR JIT backend. Compiles a C file
// (or stdin with "-") entirely in memory and runs its main() via MIR's
// code generator, resolving external symbols from the host process.
//
//   ./slimcc-mir-run [-d] file.c [args...]
//
// -d dumps the textual MIR module to stderr before running.
#include "libslimcc.h"
#include "mir-gen.h"
#include <dlfcn.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Unresolved symbols only matter if actually reached at run time (e.g. a
// declared-but-undefined function in dead code), so resolve them to a trap.
static void unresolved_symbol_trap(void) {
  fprintf(stderr, "call to unresolved symbol\n");
  abort();
}

static void *import_resolver(const char *name) {
  void *addr = dlsym(RTLD_DEFAULT, name);
  return addr ? addr : (void *)unresolved_symbol_trap;
}

// slimcc's builtin freestanding headers (stddefer.h, stdarg.h, ...) live
// next to the compiler binary, like the CLI driver expects.
static char *builtin_include_dir(const char *argv0) {
  static char buf[4096];
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", argv0);
  snprintf(buf, sizeof(buf), "%s/slimcc_headers/include", dirname(tmp));
  return buf;
}

static char *read_all(FILE *fp) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    len += fread(buf + len, 1, cap - len - 1, fp);
    if (len < cap - 1)
      break;
    buf = realloc(buf, cap *= 2);
  }
  buf[len] = '\0';
  return buf;
}

int main(int argc, char **argv, char **envp) {
  int argi = 1;
  bool dump = false;
  const char *incl[17];
  int n_incl = 0;

  incl[n_incl++] = builtin_include_dir(argv[0]);
  for (; argi < argc; argi++) {
    if (!strcmp(argv[argi], "-d")) {
      dump = true;
    } else if (!strncmp(argv[argi], "-I", 2) && n_incl < 16) {
      incl[n_incl++] = argv[argi] + 2;
    } else {
      break;
    }
  }
  if (argi >= argc) {
    fprintf(stderr, "usage: %s [-d] [-Idir...] file.c [args...]\n", argv[0]);
    return 2;
  }

  MIR_context_t ctx = MIR_init();

  slimcc_options opts = {0};
  opts.include_paths = incl;
  opts.n_include_paths = n_incl;
  if (dump)
    opts.mir_dump = stderr;

  // The first file plus any following .c files are compiled as separate
  // modules into the same context; remaining args go to the program.
  MIR_item_t main_item = NULL;
  slimcc_register_helpers(ctx);
  do {
    const char *path = argv[argi];
    char *source;
    if (!strcmp(path, "-")) {
      source = read_all(stdin);
      path = "<stdin>";
    } else {
      FILE *fp = fopen(path, "r");
      if (!fp) {
        perror(path);
        return 2;
      }
      source = read_all(fp);
      fclose(fp);
    }

    char *err = NULL;
    MIR_module_t mod = slimcc_compile(ctx, path, source, &opts, &err);
    if (!mod) {
      fprintf(stderr, "%s", err ? err : "compilation failed\n");
      free(err);
      return 1;
    }

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items); item;
         item = DLIST_NEXT(MIR_item_t, item))
      if (item->item_type == MIR_func_item && !strcmp(item->u.func->name, "main"))
        main_item = item;

    MIR_load_module(ctx, mod);
    free(source);
  } while (argi + 1 < argc && strlen(argv[argi + 1]) > 2 &&
           !strcmp(argv[argi + 1] + strlen(argv[argi + 1]) - 2, ".c") && ++argi);

  if (!main_item) {
    fprintf(stderr, "no main() found\n");
    return 1;
  }
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 2);
  {
    const char *dbg = getenv("MIR_GEN_DEBUG");
    if (dbg) {
      MIR_gen_set_debug_file(ctx, stderr);
      MIR_gen_set_debug_level(ctx, atoi(dbg));
    }
  }
  MIR_link(ctx, MIR_set_gen_interface, import_resolver);

  int (*main_fn)(int, char **, char **) = MIR_gen(ctx, main_item);
  int rc = main_fn(argc - argi, argv + argi, envp);

  MIR_gen_finish(ctx);
  MIR_finish(ctx);
  return rc;
}
