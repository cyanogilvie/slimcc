// Compiler-side leak measurement: failing compiles only (no module retained).
#include "libslimcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static const char *bad_src = "int f(void) { return undeclared_variable + 1; }\n";
static const char *ugly_src = "int g(void) { return 1 +; }\n";

static long rss_kb(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 500;
  MIR_context_t ctx = MIR_init();
  slimcc_options opts = {0};
  const char *incl[] = {"slimcc_headers/include"};
  opts.include_paths = incl;
  opts.n_include_paths = 1;
  char name[64];
  long rss_50 = 0;

  for (int i = 0; i < iters; i++) {
    char *err = NULL;
    snprintf(name, sizeof(name), "bad%d.c", i);
    if (slimcc_compile(ctx, name, bad_src, &opts, &err)) return 1;
    free(err);
    snprintf(name, sizeof(name), "ugly%d.c", i);
    if (slimcc_compile(ctx, name, ugly_src, &opts, &err)) return 1;
    free(err);
    if (i == 49) rss_50 = rss_kb();
  }
  printf("fail-only: iters=%d rss@50=%ldkB rss@end=%ldkB growth=%ldkB (%.1fkB/compile)\n",
         iters, rss_50, rss_kb(), rss_kb() - rss_50,
         (double)(rss_kb() - rss_50) / ((iters - 50) * 2));
  MIR_finish(ctx);
  return 0;
}
