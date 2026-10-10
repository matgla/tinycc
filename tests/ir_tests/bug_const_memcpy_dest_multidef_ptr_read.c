/* const_memcpy_to_dest (source/opt/flat/memory/byte_store_merge.c) rewrites
 * __aeabi_memcpy4(dst, &buf, N) into constant stores to dst and NOPs buf's
 * const fill - sound only if nothing else reads buf.  Its isolation scan only
 * sees a read when the address resolves through rse_resolve_temp_addr, which
 * gives up on a multi-def TEMP (a ternary/phi pointer).  Here *p reaches buf
 * through such a temp, so the read returns the never-written buffer. */
#include <stdio.h>

struct B { int v[12]; };

__attribute__((noinline)) int f(struct B *d, int c)
{
  struct B b;
  b.v[0] = 1; b.v[1] = 2; b.v[2] = 3; b.v[3] = 4; b.v[4] = 5; b.v[5] = 6;
  b.v[6] = 1; b.v[7] = 2; b.v[8] = 3; b.v[9] = 4; b.v[10] = 5; b.v[11] = 6;
  *d = b;
  int *p = c ? &b.v[0] : &b.v[1];
  return *p;
}

int main(void)
{
  struct B d;
  int a = f(&d, 1);
  int b = f(&d, 0);
  int c = d.v[3];
  printf("%d %d %d\n", a, b, c);
  return a * 10 + b + c * 100;
}
