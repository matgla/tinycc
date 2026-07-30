/* Guard: constant sub-word stores that fill an aligned word are merged into one
 * INT32 store — and the cases where doing so is a pessimisation are not.
 *
 * tcc_ir_opt_byte_store_merge used to handle only four constant INT8 stores, and
 * only when the destination was a direct `&global + addend`: it never built the
 * TEMP def map its own address resolver needs, so any store through a temp base
 * silently failed to resolve, and a direct `StackLoc[off]` destination was not a
 * form the resolver knew at all.  Widening it to INT16 and to those two address
 * forms is what lets a lane-by-lane `vector(8, short)` collapse to word stores.
 *
 * These cases assert values, so what they pin is that a merge never assembles
 * the word wrongly: byte order, truncation of a negative or high-bit-set lane,
 * and a word that is only partially constant and so may not be merged at all.
 * Whether a given case actually merges is a code-size question, measured by
 * scripts/regression_disasm.py rather than asserted here.
 *
 * copy_by_bytes is the shape where widening *costs* code, kept here so the
 * interaction stays documented next to the tests:
 * tcc_ir_opt_const_memcpy_to_dest replays a constant source buffer into the copy
 * destination at whatever granularity it finds the stores, and only a same-width
 * store forwards into the destination's loads.  Merging the byte stores that
 * fill such a buffer turns a fully-folded comparison chain back into eight loads
 * and eight compares (gcc.c-torture 20030408-1, which measures it).
 */
#include <stdio.h>

typedef __attribute__((vector_size(16))) short v8s;

volatile int one = 1;

/* Lane-by-lane halfword fill: lanes 2..7 are constant and pair up into words. */
int vector_lanes(void)
{
  v8s v = {(short)one, 1, 2, 3, 4, 5, 6, 7};
  short *p = (short *)&v;
  int sum = 0;
  for (int i = 0; i < 8; i++)
    sum += p[i];
  return sum;
}

/* Negative and sign-bit-set halfwords: the merged word must hold the truncated
 * 16-bit patterns, not sign-extended 32-bit ones. */
int negative_halfwords(void)
{
  short a[4];
  a[0] = -1;
  a[1] = -2;
  a[2] = (short)0x8000;
  a[3] = (short)0xFFFF;
  return a[0] + a[1] + a[2] + a[3];
}

/* Byte lanes, including a high-bit-set one. */
int byte_lanes(void)
{
  unsigned char b[4];
  b[0] = 0x11;
  b[1] = 0x22;
  b[2] = 0x80;
  b[3] = 0xFF;
  return b[0] + b[1] + b[2] + b[3];
}

/* Mixed widths filling one word: a halfword then two bytes. */
int mixed_widths(void)
{
  union { unsigned short h[2]; unsigned char b[4]; } u;
  u.h[0] = 0x1234;
  u.b[2] = 0x56;
  u.b[3] = 0x78;
  return u.b[0] + u.b[1] + u.b[2] + u.b[3];
}

/* Only three of four bytes are constant, so the word is never complete and
 * nothing may be merged. */
int partial_word(void)
{
  unsigned char b[4];
  b[0] = 1;
  b[1] = 2;
  b[2] = 3;
  b[3] = (unsigned char)one;
  return b[0] + b[1] + b[2] + b[3];
}

/* The pessimisation guard: a constant buffer copied out and then read back byte
 * by byte must still fold away completely. */
int copy_by_bytes(void)
{
  const char X[8] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
  char buffer[8];
  __builtin_memcpy(buffer, X, 8);
  if (buffer[0] != 'A' || buffer[1] != 'B' || buffer[2] != 'C' || buffer[3] != 'D' ||
      buffer[4] != 'E' || buffer[5] != 'F' || buffer[6] != 'G' || buffer[7] != 'H')
    return -1;
  return 0;
}

int main(void)
{
  int fails = 0;
  int v;

  if ((v = vector_lanes()) != 29) { printf("vector_lanes: %d\n", v); fails++; }
  if ((v = negative_halfwords()) != -32772) { printf("negative_halfwords: %d\n", v); fails++; }
  if ((v = byte_lanes()) != 0x11 + 0x22 + 0x80 + 0xFF) { printf("byte_lanes: %d\n", v); fails++; }
  if ((v = mixed_widths()) != 0x34 + 0x12 + 0x56 + 0x78) { printf("mixed_widths: %d\n", v); fails++; }
  if ((v = partial_word()) != 7) { printf("partial_word: %d\n", v); fails++; }
  if ((v = copy_by_bytes()) != 0) { printf("copy_by_bytes: %d\n", v); fails++; }

  printf("fails=%d\n", fails);
  return 0;
}
