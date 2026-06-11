// Zero-filesystem libslimcc check: no include paths, stdarg.h and
// _BitInt>64 must come from the embedded headers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libslimcc.h"
#include "mir-gen.h"

static const char *src =
  "#include <stdarg.h>\n"
  "#include <stdbool.h>\n"
  "#include <stdckdint.h>\n"
  "typedef _BitInt(128) i128;\n"
  "static int vsum(int n, ...) {\n"
  "  va_list ap; va_start(ap, n); int s = 0;\n"
  "  while (n--) s += va_arg(ap, int);\n"
  "  va_end(ap); return s;\n"
  "}\n"
  "int run(void) {\n"
  "  i128 x = (i128)1 << 101;\n"
  "  int hi = (int)((x + x) >> 100);\n"
  "  int ovf; bool b = ckd_add(&ovf, 1, 2);\n"
  "  return hi * 100 + vsum(2, 3, 4) * (!b);\n" // 4*100 + 7 = 407
  "}\n";

int main(void) {
  MIR_context_t ctx = MIR_init();
  slimcc_register_helpers(ctx);
  char *err = NULL;
  MIR_module_t mod = slimcc_compile(ctx, "zerofs.c", src, NULL, &err);
  if (!mod) { fprintf(stderr, "compile failed: %s\n", err ? err : "?"); return 1; }
  MIR_item_t run = NULL;
  for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it; it = DLIST_NEXT(MIR_item_t, it))
    if (it->item_type == MIR_func_item && !strcmp(it->u.func->name, "run")) run = it;
  MIR_load_module(ctx, mod);
  MIR_gen_init(ctx);
  MIR_link(ctx, MIR_set_gen_interface, NULL);
  int (*fn)(void) = MIR_gen(ctx, run);
  int r = fn();
  printf("run() = %d (want 407)\n", r);
  MIR_gen_finish(ctx);
  MIR_finish(ctx);
  return r != 407;
}
