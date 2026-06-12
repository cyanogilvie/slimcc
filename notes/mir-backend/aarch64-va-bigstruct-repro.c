// aarch64 MIR vararg miscompile: a large (>16-byte, by-value) struct vararg
// mixed with FP varargs desynchronizes the va_list cursors. Found 2026-06-12
// via test/function2.c struct_test130/131 (unmasked by the paramless-variadic
// fixes - the file previously failed to compile, so this never ran).
//
// Run: ./slimcc-mir-run -I/usr/include notes/mir-backend/aarch64-va-bigstruct-repro.c
//
// aarch64 (glibc AND musl, gen):              x86-64 and host gcc (correct):
//   ph: ld1=11.1 bs1=123 i=992 d1=0 d2=0        ph: ld1=2.3 bs1=123 i=33 d1=4.5
//       d3=0 d4=0 bs2=78 ld2=11.1                   d2=5.6 d3=6.7 d4=7.8 bs2=78 ld2=11.1
//   pf: ld=2.3 (correct)                        pf: ld=2.3
//   pi: bs=123 d=4.5 ld=2.3 (correct)           pi: bs=123 d=4.5 ld=2.3
//
// Note i=992 == 999 & ~7 (the qword-rounded struct size) - a cursor advanced
// by the struct size where an 8-byte step was meant, or vice versa.
//
// Named args are NOT the trigger (ph has only two named ints; a full
// G/F-struct named shape with short vararg lists works). The shorter pi list
// also works, so it needs FP varargs on both sides of the big struct.
//
// mir's own c2m cannot act as the oracle: va_arg of the big struct dies with
// "Wrong alias number" at compile time (separate c2m bug, also reproducible
// with stock upstream).
#include <stdarg.h>
#include <stdio.h>
typedef struct { char c; } G;
typedef struct { float f; } F;
typedef struct { char c[999]; } BigStruct;

int ph(int i0, int i1, ...) {
  va_list ap; va_start(ap, i1);
  printf("ph: ld1=%Lg", va_arg(ap,long double));
  printf(" bs1=%d", va_arg(ap,BigStruct).c[123]);
  printf(" i=%d", va_arg(ap,int));
  printf(" d1=%g d2=%g d3=%g d4=%g", va_arg(ap,double), va_arg(ap,double), va_arg(ap,double), va_arg(ap,double));
  printf(" bs2=%d", va_arg(ap,BigStruct).c[456]);
  printf(" ld2=%Lg\n", va_arg(ap,long double));
  va_end(ap); return 0;
}
int pf(G g0,G g1,G g2,G g3,G g4,F f0,F f1,F f2,F f3,F f4,F f5,int i0,int i1, ...) {
  va_list ap; va_start(ap, i1);
  printf("pf: ld=%Lg\n", va_arg(ap,long double));
  va_end(ap); return 0;
}
int pi(int i0, int i1, ...) {
  va_list ap; va_start(ap, i1);
  printf("pi: bs=%d d=%g ld=%Lg\n", va_arg(ap,BigStruct).c[123], va_arg(ap,double), va_arg(ap,long double));
  va_end(ap); return 0;
}
int main(void) {
  G g[] = {{1},{2},{3},{4},{5}};
  F f[] = {{1},{2},{3},{4},{5},{6}};
  BigStruct s = {.c[123] = 123, .c[456] = 78};
  ph(11, 22, (long double)2.3, s, (int)33, (double)4.5, (double)5.6, (double)6.7, (double)7.8, s, (long double)11.1);
  pf(g[0],g[1],g[2],g[3],g[4],f[0],f[1],f[2],f[3],f[4],f[5],11,22, (long double)2.3);
  pi(11, 22, s, (double)4.5, (long double)2.3);
  return 0;
}
