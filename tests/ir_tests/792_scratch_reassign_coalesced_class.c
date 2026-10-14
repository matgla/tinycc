/* Regression lock for codegen's phase-3 scratch fixup
 * (try_reassign_scratch_conflict): it must not move one member of a
 * graph-coalesced register class, a phi end whose copy was elided, or any
 * interval sharing its register with another claimant -- doing so splits a
 * value whose copies were erased.  Many loop-carried values plus
 * scratch-hungry ops (64-bit multiply, division, indexed stores) force the
 * fixup to fire.  Checksum agrees with gcc -O0/-O2.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

__attribute__((noinline)) static unsigned work(unsigned a, unsigned b, unsigned c, unsigned d, unsigned *arr)
{
  unsigned x = a, y = b, z = c, w = d, p = a ^ d, q = b + c, cs = 7u;
  for (unsigned i = 0; i < 9u; i++)
  {
    unsigned t = x;
    unsigned long long m = (unsigned long long)x * (y | 1u);
    x = y + (unsigned)(m >> 32);
    y = z ^ (unsigned)m;
    z = w + t;
    w = p / ((q & 15u) | 1u);
    p = q ^ t;
    q = (unsigned)(m >> 7) + i;
    arr[(x ^ y) & 7u] = z + w;
    if ((i & 1u) && x > y)
    {
      unsigned s = x;
      x = y;
      y = s;
    }
    cs = csmix(cs, x ^ p);
    cs = csmix(cs, y + q);
  }
  return csmix(csmix(cs, z), w);
}

int main(void)
{
  unsigned arr[8] = {0};
  unsigned r = work(0x12345678u, 0x9abcdef1u, 0x0fedcba9u, 0x87654321u, arr);
  for (unsigned k = 0; k < 8u; k++)
    r = csmix(r, arr[k]);
  printf("checksum=%08x\n", r);
  return 0;
}
