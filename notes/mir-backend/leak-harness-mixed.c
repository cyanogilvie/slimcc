// Leak/state audit: compile the same sources repeatedly in one process,
// alternating success and failure paths.
#include "libslimcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static const char *bad_src =
  "int f(void) { return undeclared_variable + 1; }\n";

static const char *ugly_src =
  "int g(void) { return 1 +; }\n"; // syntax error mid-expression

static long rss_kb(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 200;
  MIR_context_t ctx = MIR_init();
  slimcc_options opts = {0};
  const char *incl[] = {"slimcc_headers/include"};
  opts.include_paths = incl;
  opts.n_include_paths = 1;
  char name[64];

  long rss_at_50 = 0;
  for (int i = 0; i < iters; i++) {
    char *err = NULL;
    snprintf(name, sizeof(name), "good%d.c", i);
    MIR_module_t m = slimcc_compile(ctx, name, good_src, &opts, &err);
    if (!m) { fprintf(stderr, "unexpected failure: %s", err); return 1; }

    snprintf(name, sizeof(name), "bad%d.c", i);
    if (slimcc_compile(ctx, name, bad_src, &opts, &err)) { fprintf(stderr, "expected failure\n"); return 1; }
    if (!err || !strstr(err, "undeclared")) { fprintf(stderr, "bad diag: %s\n", err ? err : "(null)"); return 1; }
    free(err);

    snprintf(name, sizeof(name), "ugly%d.c", i);
    if (slimcc_compile(ctx, name, ugly_src, &opts, &err)) { fprintf(stderr, "expected failure\n"); return 1; }
    free(err);

    if (i == 49) rss_at_50 = rss_kb();
  }
  printf("iters=%d rss@50=%ldkB rss@end=%ldkB growth=%ldkB\n", iters, rss_at_50, rss_kb(),
         rss_kb() - rss_at_50);
  MIR_finish(ctx);
  return 0;
}
