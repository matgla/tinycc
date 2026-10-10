/* ssa:setif_mask_fold and barrel-shift-annotated compares.
 *
 * The fold rewrites `CMP setif_bool, #k` into one compare of the operands the
 * SETIF's own CMP read.  When barrel-shift fusion had already folded a shift
 * into that CMP's src2 (`cmp.w r2, r1, asr #8`), the shift lives in a side
 * table keyed by the CMP's orig_index and is not part of the operand: copying
 * the operands to the outer CMP dropped it (`cmp r2, r1`).  f_sar3 is wrong on
 * the plain base; f_sext24 becomes reachable once ext_elim removes the int8
 * re-extension of the SETIF result.  The outer CMP can carry an annotation of
 * its own too (see 869). */
#include <stdint.h>
#include <stdio.h>

#define NI __attribute__((noinline))

uint32_t g32[9];

NI int32_t f_sext24(int i, int j)
{
  return 1 != (int8_t)(((int32_t)g32[i] < ((int32_t)(g32[j] << 8) >> 8)));
}

NI int32_t f_sar3(int i, int j)
{
  return 0 == ((int32_t)g32[i] < ((int32_t)g32[j] >> 3));
}

NI int32_t f_lsr5(int i, int j)
{
  return 0 != (int16_t)((g32[i] >= (g32[j] >> 5)));
}

NI int32_t f_shl4(int i, int j)
{
  return 1 == (int8_t)(((int32_t)g32[i] > (int32_t)(g32[j] << 4)));
}

static const int32_t V[] = {-5, -1, 0, 1, 2, 100, -100, 0x7fffffff, (int32_t)0x80000000};

static void row(const char *name, int32_t (*f)(int, int))
{
  printf("%s", name);
  for (int i = 0; i < 9; i++)
  {
    printf(" ");
    for (int j = 0; j < 9; j++)
      printf("%d", (int)f(i, j));
  }
  printf("\n");
}

int main(void)
{
  for (int i = 0; i < 9; i++)
    g32[i] = (uint32_t)V[i] * 0x1001u;
  row("sext24", f_sext24);
  row("sar3", f_sar3);
  row("lsr5", f_lsr5);
  row("shl4", f_shl4);
  return 0;
}
