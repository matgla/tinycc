/* docs/bugs/nested-fn-cleanup-call-no-static-chain.md
 * A GNU nested function used as __attribute__((cleanup)) handler is called
 * without the static chain: its captures read and write garbage.
 *
 *   cd tests/ir_tests && python3 run.py \
 *     -c ../../docs/bugs/repro/nested-fn-cleanup-call-no-static-chain.c --cflags=-O0
 *
 * Expected (gcc): 52 4        tcc, every level: 50 0 */
#include <stdio.h>
int acc;
__attribute__((noinline)) int cleanup_nf(int a)
{
  void done(int *p) { acc += *p * 10 + a; }
  {
    int x __attribute__((cleanup(done))) = 4;
    x++;
  }
  return acc;
}
__attribute__((noinline)) int cleanup_nf2(int a)
{
  int acc2 = 0;
  void done(int *p) { acc2 += *p; }
  {
    int x __attribute__((cleanup(done))) = 4;
  }
  return acc2;
}
int main(void) { printf("%d %d\n", cleanup_nf(2), cleanup_nf2(2)); return 0; }
