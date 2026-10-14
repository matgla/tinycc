/* dead_vla_struct deleted a VLA that was still read: an element passed to an
 * inlined function becomes `V <-- T***DEREF*** [STORE]` (the argument copied
 * into the inline body's parameter variable), and the pass's STORE branch only
 * checked for the VLA pointer itself escaping, never for a read through it.
 * Found by the gen_c.py "hazard" fuzz profile (23 of 30 VLA seeds at -O2/-Os). */
#include <stdio.h>

static unsigned mix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

__attribute__((noinline)) unsigned pick(unsigned x) { return x * 2654435761u; }

int main(void)
{
  unsigned seed = 3537882513u, cs = 0x12345678u;
  {
    unsigned n = (pick(seed) & 7u) + 1u;
    unsigned v[n];
    for (unsigned k = 0u; k < n; k++)
      v[k] = seed * (k + 3u);
    cs = mix(cs, v[pick(cs) % n]);
  }
  printf("%08x\n", cs);
  return 0;
}
