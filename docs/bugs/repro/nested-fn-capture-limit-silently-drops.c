/* docs/bugs/nested-fn-capture-limit-silently-drops.md
 * A nested function that names more than MAX_CAPTURED_VARS (32) of its
 * parent's locals: the captures past the 32nd are dropped, and the body
 * fails to compile.
 *
 *   cd tests/ir_tests && python3 run.py \
 *     -c ../../docs/bugs/repro/nested-fn-capture-limit-silently-drops.c --cflags=-O0
 *
 * Expected (gcc): 281        tcc, every level: error: 'v40' undeclared (the 33rd capture) */
#include <stdio.h>
#define L8(b) X(b##0) X(b##1) X(b##2) X(b##3) X(b##4) X(b##5) X(b##6) X(b##7)
#define ALL L8(v0) L8(v1) L8(v2) L8(v3) L8(v4)
__attribute__((noinline)) int many(int x)
{
#define X(n) int n = x + (int)sizeof(#n);
  ALL
#undef X
#define X(n) + n
  int nf(int k) { return k ALL; }
#undef X
  return nf(1);
}
int main(void) { printf("%d\n", many(3)); return 0; }
