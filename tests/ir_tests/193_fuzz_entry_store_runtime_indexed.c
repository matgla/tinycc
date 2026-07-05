/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=294).
 * entry_store_prop forwarded a stale array initializer past an entry-BB RUNTIME-indexed store `arr[i]=x` (Phases 1.5/2.5 only scanned after the entry BB). Fix: dedicated entry-BB runtime-store invalidation via a separate rt_base map (ir/opt_memory.c).
 * tcc -O0 was always correct; the bug appeared at -O1/-O2.  Expected checksum
 * is gcc -m32 -funsigned-char (ARM ABI: unsigned char, 32-bit long).
 */
#include <stdio.h>

/* Rolling checksum mix (all unsigned -> fully defined). */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}


static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)((-((unsigned)(pa) | 0u))) * (unsigned)((((unsigned)((((unsigned)(1092850002u) & 1u) ? (unsigned)(pa) : (unsigned)(pb))) & 1u) ? (unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(pb) : (unsigned)(((unsigned)(pb) ^ lr)))) : (unsigned)(((unsigned)(3670645944u) ^ (unsigned)(pb)))))));
  lr = (unsigned)(2744683560u);
  return (unsigned)(pb) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s2 = (short)(664887057u & 0xffff);
  long s3 = (long)(1611006594u & 0xffffffff);
  unsigned u4 = 3702401155u;
  unsigned u5 = 2737334135u;
  unsigned arr6[8] = { 4149856122u, 3176998962u, 288645242u, 4031511239u, 3896007099u, 2821118338u, 1096709554u, 629331030u };
  unsigned arr7[8] = { 2932966655u, 3294567214u, 3247947290u, 833450885u, 1045198364u, 2474731142u, 3000214380u, 1747839011u };

  u4 = (unsigned)(((unsigned)((-((unsigned)(1074479421u) | 0u))) / ((unsigned)((~((unsigned)(u4) | 0u))) | 1u))) & 0xffffffffu;
  u5 = (unsigned)(((unsigned)(((unsigned)(helper1((~((unsigned)(arr6[((unsigned)(3275099801u) & 7u)]) | 0u)), (-((unsigned)(arr6[((unsigned)(u4) & 7u)]) | 0u)))) >> ((unsigned)(((unsigned)(helper1(u5, 2533724u)) / ((unsigned)(((unsigned)((unsigned)(s3)) < ((unsigned)(570305076u) ^ cs))) | 1u))) & 31u))) | (unsigned)(((unsigned)((~((unsigned)(u5) | 0u))) - (unsigned)(((unsigned)((~((unsigned)(139432095u) | 0u))) + (unsigned)(((unsigned)(u5) | (unsigned)(2808804224u))))))))) & 0xffffffffu;
  arr7[((unsigned)(u5) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) <= ((unsigned)(arr6[((unsigned)(881937233u) & 7u)]) ^ cs))) & (unsigned)(((unsigned)(((unsigned)(1973552777u) | (unsigned)(u4))) % ((unsigned)((-((unsigned)(arr6[((unsigned)(2604534666u) & 7u)]) | 0u))) | 1u))))) % ((unsigned)((((unsigned)(((unsigned)(4252061529u) & (unsigned)(((unsigned)((unsigned)(s3)) > ((unsigned)(3896859450u) ^ cs))))) & 1u) ? (unsigned)(((unsigned)(678115427u) & (unsigned)(((unsigned)(arr6[((unsigned)(u5) & 7u)]) / ((unsigned)(u4) | 1u))))) : (unsigned)(u4))) | 1u)));
  cs = csmix(cs, (unsigned)(arr7[((unsigned)(u4) & 7u)]));

  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr6[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
