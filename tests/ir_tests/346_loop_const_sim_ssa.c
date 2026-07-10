/* Loop constant simulation across the legacy -> ssa:loop_const_sim migration.
 *
 * Pins three shapes the LCS engine handles and the unroller does not, so the
 * result must be identical at -O0/-O1 (pass off) and -O2/-Os (pass on),
 * whichever driver (legacy Phase 4e or ssa:loop_const_sim) folds them:
 *
 *   A. a soft-float (__aeabi_d*) accumulator loop — calls in the body, so the
 *      unroller declines it; LCS evaluates the helpers on the host;
 *   B. a second loop consuming loop A's residual `acc` (cascade — folds across
 *      the 4-round driver with no interleaved cleanup);
 *   C. a runtime-bounded control loop reading a volatile trip count — a memory
 *      read in the loop, so LCS must NOT fold it.
 *
 * FP constants are exact binary fractions so host-side folding matches the
 * ARM soft-float runtime bit-for-bit.
 */
#include <stdio.h>

static unsigned mix(unsigned h, double v)
{
  union { double d; unsigned long long u; } bits;
  bits.d = v;
  h ^= (unsigned)(bits.u & 0xffffffffu);
  h = (h << 7) | (h >> 25);
  h ^= (unsigned)(bits.u >> 32);
  return h * 2654435761u;
}

int main(void)
{
  double acc = 0.5;
  for (int i = 0; i < 6; i++)
    acc = acc + (double)i * 1.25;

  double scaled = acc;
  for (int j = 0; j < 4; j++)
    scaled = scaled * 1.5 - 0.25;

  volatile int limit = 5;
  int guard = limit;
  int sum = 0;
  for (int k = 0; k < guard; k++)
    sum += k * 3 + 1;

  unsigned cs = 0x9e3779b9u;
  cs = mix(cs, acc);
  cs = mix(cs, scaled);
  cs ^= (unsigned)sum;

  printf("cs=%08x acc=%d scaled=%d sum=%d\n", cs, (int)acc, (int)scaled, sum);
  return 0;
}
