// libslimcc string literals: literals and adjacent-literal concatenations
// bigger than an arena pool (which used to hit internal_error), and the
// concatenation buffer released with the compile (it used to be a raw calloc,
// leaked by every compile: check with valgrind --leak-check=full).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libslimcc.h"
#include "mir-gen.h"

#define BIG 9000	// > ARENA_POOL_SIZE

static char *make_src(void) {
  size_t cap = 4 * BIG + 1024;
  char *src = malloc(cap);
  char *p = src;
  p += sprintf(p, "#include <stddef.h>\n");
  p += sprintf(p, "static const char single[] = \"");
  memset(p, 'x', BIG); p += BIG;
  p += sprintf(p, "\";\nstatic const char joined[] = \"");
  memset(p, 'y', BIG); p += BIG;
  p += sprintf(p, "\" \"");
  memset(p, 'z', BIG); p += BIG;
  p += sprintf(p, "\";\n"
    "static const int wide[] = L\"ab\" \"cd\" L\"ef\";\n"
    "static const char small[] = \"a\" \"b\" \"c\";\n"
    "int run(void) {\n"
    "  if (sizeof single != " "%d" " + 1 || single[" "%d" " - 1] != 'x') return 1;\n"
    "  if (sizeof joined != 2 * " "%d" " + 1 || joined[" "%d" "] != 'z' || joined[" "%d" " - 1] != 'y') return 2;\n"
    "  if (sizeof wide / sizeof wide[0] != 7 || wide[2] != 'c' || wide[5] != 'f' || wide[6]) return 3;\n"
    "  if (sizeof small != 4 || small[2] != 'c') return 4;\n"
    "  return 0;\n"
    "}\n", BIG, BIG, BIG, BIG, BIG);
  return src;
}

int main(int argc, char **argv) {
  int iterations = argc > 1 ? atoi(argv[1]) : 3;
  char *src = make_src();
  int failed = 0;

  for (int i = 0; i < iterations && !failed; i++) {
    MIR_context_t ctx = MIR_init();
    slimcc_register_helpers(ctx);
    char *err = NULL;
    MIR_module_t mod = slimcc_compile(ctx, "strlit.c", src, NULL, &err);
    if (!mod) {
      fprintf(stderr, "compile failed: %s\n", err ? err : "?");
      free(err);
      MIR_finish(ctx);
      failed = 1;
      break;
    }
    MIR_item_t run = NULL;
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it; it = DLIST_NEXT(MIR_item_t, it))
      if (it->item_type == MIR_func_item && !strcmp(it->u.func->name, "run")) run = it;
    MIR_load_module(ctx, mod);
    MIR_gen_init(ctx);
    MIR_link(ctx, MIR_set_gen_interface, NULL);
    int (*fn)(void) = MIR_gen(ctx, run);
    int r = fn();
    if (r) {
      printf("iteration %d: run() = %d (want 0)\n", i, r);
      failed = 1;
    }
    MIR_gen_finish(ctx);
    MIR_finish(ctx);
  }

  free(src);
  slimcc_shutdown();
  if (!failed) printf("ok: %d compiles\n", iterations);
  return failed;
}
