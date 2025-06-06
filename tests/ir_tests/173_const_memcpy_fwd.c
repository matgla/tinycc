/* Regression guard for tcc_ir_opt_const_memcpy_to_dest (ir/opt_memory.c).
 *
 * That pass rewrites a constant-filled non-escaping stack buffer copied to a
 * destination pointer by an alignment-guaranteeing AEABI mem* helper into
 * direct wide constant stores (the GCC vector_size const-store idiom, e.g.
 * pr60502).  These cases verify the RESULT bytes are still correct:
 *
 *   all_ff   : `*x |= *x ^ {-1,...}`  folds to the constant all-0xFF — the
 *              transform must store 0xFF regardless of the prior contents.
 *   set_words: assignment of a 4-word constant vector with DISTINCT words —
 *              catches any byte-image / endianness / width bug in the rewrite.
 *   xor_in   : the stored value depends on a second pointer, so it is NOT a
 *              compile-time constant — the pass must leave it alone, and the
 *              element-wise xor result must be correct.
 */
#include <stdio.h>

typedef signed char v16i8 __attribute__((vector_size(16)));
typedef unsigned int v4u32 __attribute__((vector_size(16)));

__attribute__((noinline)) void all_ff(v16i8 *x)
{
  v16i8 m = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  *x |= *x ^ m;
}

__attribute__((noinline)) void set_words(v4u32 *x)
{
  v4u32 v = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
  *x = v;
}

__attribute__((noinline)) void xor_in(v16i8 *x, v16i8 *y)
{
  *x = *x ^ *y;
}

int main(void)
{
  static v16i8 a;
  unsigned char *pa = (unsigned char *)&a;
  for (int i = 0; i < 16; i++)
    pa[i] = (unsigned char)(i * 13 + 7);
  all_ff(&a);
  for (int i = 0; i < 16; i++)
    if (pa[i] != 0xFF)
    {
      printf("FAIL all_ff %d=%02x\n", i, pa[i]);
      return 1;
    }

  static v4u32 b;
  set_words(&b);
  unsigned int *pb = (unsigned int *)&b;
  if (pb[0] != 0x11223344u || pb[1] != 0x55667788u ||
      pb[2] != 0x99aabbccu || pb[3] != 0xddeeff00u)
  {
    printf("FAIL set_words %08x %08x %08x %08x\n", pb[0], pb[1], pb[2], pb[3]);
    return 2;
  }

  static v16i8 c, d;
  unsigned char *pc = (unsigned char *)&c, *pd = (unsigned char *)&d;
  for (int i = 0; i < 16; i++)
  {
    pc[i] = (unsigned char)(i * 7);
    pd[i] = (unsigned char)(i * 3 + 1);
  }
  xor_in(&c, &d);
  for (int i = 0; i < 16; i++)
  {
    unsigned char e = (unsigned char)((i * 7) ^ (i * 3 + 1));
    if (pc[i] != e)
    {
      printf("FAIL xor %d=%02x exp %02x\n", i, pc[i], e);
      return 3;
    }
  }

  printf("OK\n");
  return 0;
}
