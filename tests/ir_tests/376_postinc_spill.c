/* Post-index writeback addressing with a spilled base: many accumulators live
 * across the loop force register pressure high enough that the writeback
 * pointer may spill.  The emitter must store the bumped base back to its
 * spill slot (the revival condition of docs/plan_legacy_loop_postinc_fusion_ssa.md);
 * without it the pointer never advances and the loop reads arr[0] forever. */
#include <stdio.h>

int arr[64];

int main(void)
{
  for (int i = 0; i < 64; i++)
    arr[i] = (i * 7) ^ (i >> 2);

  int s0 = 0, s1 = 1, s2 = 2, s3 = 3, s4 = 4, s5 = 5;
  int s6 = 6, s7 = 7, s8 = 8, s9 = 9, s10 = 10, s11 = 11;

  for (int i = 0; i < 64; i++)
  {
    int v = arr[i];
    s0 += v;
    s1 += v ^ i;
    s2 += s0;
    s3 += s1;
    s4 += s2;
    s5 += s3;
    s6 += s4;
    s7 += s5;
    s8 += s6;
    s9 += s7;
    s10 += s8;
    s11 += s9;
  }

  printf("%d %d %d %d %d %d %d %d %d %d %d %d\n", s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11);
  return 0;
}
