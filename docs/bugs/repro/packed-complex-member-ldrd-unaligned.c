/* docs/bugs/packed-complex-member-ldrd-unaligned.md
 * A _Complex float member at offset 1 of a packed struct is loaded with
 * LDRD, which faults on an unaligned address (CFSR UNALIGNED), every level.
 * Expected (gcc): "55 55" */
#include <complex.h>
#include <stdio.h>
typedef struct __attribute__((packed)) { char c; _Complex float z; } PK;
__attribute__((noinline)) float im(_Complex float c) { return cimagf(c); }
__attribute__((noinline)) _Complex float id(_Complex float c) { return c; }
__attribute__((noinline)) float viapk(PK *p) { return im(p->z); }
int main(void)
{
  PK pk;
  pk.c = 1;
  pk.z = id(4.0f + 5.5f * I);
  printf("%d %d\n", (int)(viapk(&pk) * 10), (int)(im(pk.z) * 10));
  return 0;
}
