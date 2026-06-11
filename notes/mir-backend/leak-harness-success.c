// Success-path leak measurement: fresh consumer context per iteration so
// retained modules are freed and only compiler-side leakage remains.
#include "libslimcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

static const char *good_src =
  "#include <stddefer.h>\n"
  "int printf(const char *, ...);\n"
  "static int counter;\n"
  "struct P { int x, y; };\n"
  "static int area(struct P a, struct P b) { return (b.x-a.x)*(b.y-a.y); }\n"
  "int work(int n) {\n"
  "  int acc = 0;\n"
  "  defer acc += counter;\n"
  "  for (int i = 0; i < n; i++) {\n"
  "    struct P p1 = {i, i+1}, p2 = {2*i, 3*i};\n"
  "    acc += area(p1, p2);\n"
  "  }\n"
  "  counter++;\n"
  "  return acc;\n"
  "}\n";

static long rss_kb(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 500;
  slimcc_options opts = {0};
  const char *incl[] = {"slimcc_headers/include"};
  opts.include_paths = incl;
  opts.n_include_paths = 1;
  long rss_50 = 0;

  for (int i = 0; i < iters; i++) {
    MIR_context_t ctx = MIR_init();
    char *err = NULL;
    MIR_module_t m = slimcc_compile(ctx, "work.c", good_src, &opts, &err);
    if (!m) { fprintf(stderr, "fail: %s", err); return 1; }
    MIR_finish(ctx);
    if (i == 49) rss_50 = rss_kb();
  }
  printf("success: iters=%d rss@50=%ldkB rss@end=%ldkB growth=%ldkB (%.1fkB/compile)\n",
         iters, rss_50, rss_kb(), rss_kb() - rss_50,
         (double)(rss_kb() - rss_50) / (iters - 50));
  MIR_finish(MIR_init()); // keep linkage shape consistent
  return 0;
}
