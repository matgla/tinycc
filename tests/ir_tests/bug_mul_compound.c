#include <stdio.h>

/* Use volatile to prevent optimization */
volatile unsigned long long v;

int main(void)
{
  v = 1;

  /* First few multiplications should fit in 32 bits */
  v *= 10; /* v = 10 */
  v *= 10; /* v = 100 */
  v *= 10; /* v = 1000 */
  v *= 10; /* v = 10000 */
  v *= 10; /* v = 100000 */
  v *= 10; /* v = 1000000 */
  v *= 10; /* v = 10000000 */
  v *= 10; /* v = 100000000 */
  v *= 10; /* v = 1000000000 */
  v *= 10; /* v = 10000000000 - this exceeds 32 bits! */

  /* 10^10 = 10,000,000,000 = 0x2_540BE400 */
  /* hi = 2, lo = 0x540BE400 */

  unsigned lo = (unsigned)v;
  unsigned hi = (unsigned)(v >> 32);

  printf("After 10 multiplications by 10:\n");
  printf("v = 0x%08x%08x\n", hi, lo);
  printf("expect: v = 0x00000002540be400 (10^10 = 10000000000)\n");

  if (v == 10000000000ULL)
  {
    printf("PASS\n");
    return 0;
  }
  else
  {
    printf("FAIL v=%llu\n", v);
    return 1;
  }
}
