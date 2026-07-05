/* Regression for differential-fuzz seed 2657: wrong-code at -O2 only.
 *
 * Root cause: ssa_opt_load_cse's stack-store forwarding.  A STORE_INDEXED
 * through a stack array base (`Addr[StackLoc[B]] <-- v STORE_INDEXED idx`) with
 * a RUNTIME index only invalidated the single sstore-forward entry at the base
 * offset B, leaving the initializer values for the sibling slots forwardable.
 * The fully-unrolled `for k cs=csmix(cs,arr5[k])` then forwarded arr5[k]'s
 * initializer for every k != B even though `arr5[runtime]=v` could have
 * overwritten any slot.
 *
 * Fix: a runtime-indexed stack STORE_INDEXED drops all stack-store and
 * indexed-load forwarding state (constant index invalidates just that slot).
 * Ground truth (gcc -m32 -funsigned-char): checksum=4a152f38 (== tcc -O0/-O1).
 * Buggy -O2 produced e72b1f82.
 */
#include <stdio.h>

/* Rolling checksum mix (all unsigned -> fully defined). */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}


struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s1 = (char)(797624347u & 0xff);
  short s2 = (short)(1878913767u & 0xffff);
  unsigned u3 = 3161427885u;
  unsigned u4 = 1875022957u;
  unsigned arr5[8] = { 2270474074u, 360813467u, 3099058800u, 2585791491u, 936605977u, 783854144u, 913542789u, 4084505692u };

  if ((unsigned)((unsigned)(s2)) & 1u) {
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u3) >> ((unsigned)(((unsigned)(3366674771u) | (unsigned)((-((unsigned)(u4) | 0u))))) & 31u))) + (unsigned)(((unsigned)(((unsigned)(u3) + (unsigned)(((unsigned)(1728052905u) & (unsigned)(2790186940u))))) * (unsigned)(3718974460u))))));
    u3 = (unsigned)(((unsigned)((-((unsigned)(((unsigned)((((unsigned)(arr5[((unsigned)(3483781502u) & 7u)]) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)(arr5[((unsigned)(u3) & 7u)]))) / ((unsigned)((-((unsigned)(2622387733u) | 0u))) | 1u))) | 0u))) < ((unsigned)(2458191773u) ^ cs))) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(((unsigned)(u3) - (unsigned)(3747736293u))));
    arr5[((unsigned)(3034078099u) & 7u)] = (unsigned)(u3);
  } else {
    if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) | (unsigned)(u3))) - (unsigned)((unsigned)(s1)))) & (unsigned)(577155420u))) <= ((unsigned)(u3) ^ cs))) & 1u) {
      arr5[((unsigned)(1034794133u) & 7u)] = (unsigned)((-((unsigned)((((unsigned)(((unsigned)(((unsigned)(4102031971u) * (unsigned)(4220406498u))) << ((unsigned)(arr5[((unsigned)(u3) & 7u)]) & 31u))) & 1u) ? (unsigned)(((unsigned)(u4) * (unsigned)(((unsigned)(u4) | (unsigned)((unsigned)(s1)))))) : (unsigned)(3186255621u))) | 0u)));
      cs = csmix(cs, (unsigned)(2576176584u));
      u3 = (unsigned)(u3) & 0xffffffffu;
    }
    arr5[((unsigned)(u4) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) >> ((unsigned)(u3) & 31u))) > ((unsigned)(((unsigned)(234701214u) % ((unsigned)(945239439u) | 1u))) ^ cs))) / ((unsigned)(((unsigned)(2717979297u) - (unsigned)(((unsigned)(472024446u) >= ((unsigned)(u4) ^ cs))))) | 1u))) == ((unsigned)(((unsigned)((((unsigned)(((unsigned)(1572556566u) / ((unsigned)(u4) | 1u))) & 1u) ? (unsigned)(((unsigned)(u4) ^ (unsigned)((unsigned)(s1)))) : (unsigned)(u4))) / ((unsigned)(((unsigned)(u4) | (unsigned)(((unsigned)(u3) % ((unsigned)(arr5[((unsigned)(u4) & 7u)]) | 1u))))) | 1u))) ^ cs)));
  }
  u4 = (unsigned)(((unsigned)(((unsigned)(4243254910u) & (unsigned)(arr5[((unsigned)(u3) & 7u)]))) | (unsigned)(u4))) & 0xffffffffu;
  u3 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) >> ((unsigned)(644113764u) & 31u))) + (unsigned)(1795854325u))) % ((unsigned)(((unsigned)(u3) * (unsigned)(arr5[((unsigned)(645775732u) & 7u)]))) | 1u))) < ((unsigned)(u3) ^ cs))) & 0xffffffffu;
  u3 = (unsigned)((((unsigned)(((unsigned)(arr5[((unsigned)(u4) & 7u)]) & (unsigned)(375716194u))) & 1u) ? (unsigned)(2553527997u) : (unsigned)((~((unsigned)(((unsigned)(u4) & (unsigned)(((unsigned)(1895134315u) << ((unsigned)(u4) & 31u))))) | 0u))))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(2984296331u));
  arr5[((unsigned)(u4) & 7u)] = (unsigned)(u4);

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr5[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
