#include <stdio.h>

int main(void)
{
  unsigned long long v = 0x100000003ULL; /* Has bits in both hi and lo words */
  unsigned long long r = v * 10ULL;      /* Multiply by constant */

  printf("v = 0x%08x%08x\n", (unsigned)(v >> 32), (unsigned)v);
  printf("r = 0x%08x%08x\n", (unsigned)(r >> 32), (unsigned)r);

  /* Expected: v * 10 = 0x100000003 * 10 = 0xA0000001E = 0x00000000A:0000001E */
  /* Actually: 0x100000003 * 10 = 0xA0000001E (10737418270) */
  /* In hex: 0x00000002:8000001E (wrong) vs 0x00000000:8000001E (wrong without hi) */
  /* Correct: 0x700000015 if we use mul_u function */

  /* Wait, recalculating:
     v = 0x1_00000003 = 4294967299
     v * 10 = 42949672990 = 0xA_00000016 (should be 0x00000000A:00000016)
     High word should be 0x0000000A, low word should be 0x00000016
  */
  printf("expect hi=0000000a lo=00000016\n");

  if (r == 42949672990ULL)
  {
    printf("PASS\n");
  }
  else
  {
    printf("FAIL r=%llu expected 42949672990\n", r);
  }

  return 0;
}
