/* Regression for fuzz bitfield seed 163176: store-load forwarding must not
 * reuse stale packed bitfield container values across unrolled RMW stores. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
struct BF {
};
struct BFP {
  unsigned b0 : 11;
  unsigned b1 : 5;
  unsigned b2 : 4;
} __attribute__((packed));
int main(void)
{
  unsigned cs = 0x12345678u;
  long s1 = (long)(1522537049u & 0xffffffff);
  unsigned u2 = 2418971206u;
  unsigned u3 = 3528068u;
  unsigned u4 = 2985692777u;
  unsigned u5 = 429563221u;
  unsigned u6 = 620765581u;
  unsigned arr7[8] = { 1987183210u, 2102288339u, 262328146u, 3117938350u, 463924030u, 368064296u, 3993611131u, 1595371636u };
  struct S st8 = { 4257599373u, 3715346329u, 797641873u };
  struct BFP bf9 = { 0u, 0u, 0u };
  struct BFP bf10 = { 0u, 0u, 0u };
  for (unsigned g12 = 0u; g12 < 1u; g12++) {
    unsigned i11 = g12;
    cs = csmix(cs, i11);
    { unsigned g14 = 0u;
      while (g14 < 1u) {
        unsigned i13 = g14;
        cs = csmix(cs, i13);
        bf9.b1 = (unsigned)(u2) & ((1u << 5) - 1u);
        bf9.b0 = (unsigned)(u4) & ((1u << 11) - 1u);
        cs = csmix(cs, (unsigned)(((unsigned)(3999454505u) >> ((unsigned)(((unsigned)(3556383552u) - (unsigned)((~((unsigned)(((unsigned)(u2) - (unsigned)(1442617774u))) | 0u))))) & 31u))));
        g14++;
      }
    }
  }
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, (unsigned)s1);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  cs = csmix(cs, bf9.b0);
  cs = csmix(cs, bf9.b1);
  cs = csmix(cs, bf9.b2);
  cs = csmix(cs, bf10.b0);
  cs = csmix(cs, bf10.b1);
  cs = csmix(cs, bf10.b2);
  printf("checksum=%08x\n", cs);
  return 0;
}
