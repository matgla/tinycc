/* __builtin_constant_p of a comparison.  The "local vreg with one constant
 * definition" shortcut ran on any value with a vreg, including a comparison
 * result (VT_CMP / VT_JMP): there vr is the stale vreg of the left operand, so
 * `k < n` with `int k = 5` folded to 1, and vtop->sym is the cmp_op/cmp_r
 * union member, so `vtop->sym->a.addrtaken` dereferenced a wild heap address
 * (valgrind: uninitialised read in unary_primary; intermittent segfault under
 * ASLR).  Shape from gcc.c-torture/compile/20110902.c, inside an
 * always_inline body with a statement expression on the other arm. */
#include <stdio.h>

static inline __attribute__((always_inline)) int f(unsigned int n, unsigned int size)
{
  return (__builtin_constant_p(size != 0 && n > ~0 / size)
              ? !!(size != 0 && n > ~0 / size)
              : ({
                  static unsigned int count[2] = {0, 0};
                  int r = !!(size != 0 && n > ~0 / size);
                  count[r]++;
                  r;
                }));
}

__attribute__((noinline)) int g(unsigned int size) { return f(size / 4096, 4); }
__attribute__((noinline)) int g2(unsigned int n) { return f(n, 4); }

__attribute__((noinline)) int cmp_lt(int n)
{
  int k = 5;
  return __builtin_constant_p(k < n);
}

__attribute__((noinline)) int cmp_ne(int n)
{
  int k = 5;
  return __builtin_constant_p(k != n);
}

__attribute__((noinline)) int cmp_and(int n)
{
  int k = 5;
  return __builtin_constant_p(k && n);
}

__attribute__((noinline)) int cmp_or(int n)
{
  int k = 0;
  return __builtin_constant_p(k || n);
}

int main(void)
{
  printf("g=%d %d %d\n", g(0), g(4096 * 7), g(0xffffffffu));
  printf("g2=%d %d %d\n", g2(0), g2(0x3fffffffu), g2(0x40000000u));
  printf("bcp=%d %d %d %d\n", cmp_lt(9), cmp_ne(9), cmp_and(9), cmp_or(9));
  return 0;
}
