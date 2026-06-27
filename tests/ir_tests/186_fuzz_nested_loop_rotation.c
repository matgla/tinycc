/* Regression test for loop-rotation wrong-code on NESTED loops.
 *
 * Verbatim fuzz repro (gen_c.py seed=49).  main() has an outer `for g6<11` whose
 * body contains an inner `for g8<4`, both accumulating into the rolling hash
 * `cs`.  When BOTH loops were rotated, a later pass miscompiled the doubly-
 * rotated nested shape (-O2 gave checksum=fdb6186e instead of 0005b6d8);
 * rotating either loop alone was correct.  Fixed by declining to rotate a loop
 * nested inside an already-rotated loop (try_rotate_loop, ir/opt_loop_utils.c).
 * Expected checksum (gcc -O0/-O1/-O2 all agree): 0005b6d8.
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
  long s1 = (long)(474614218u & 0xffffffff);
  char s2 = (char)(213348186u & 0xff);
  unsigned u3 = 1185099586u;
  unsigned u4 = 1594242464u;

  u3 = (unsigned)((-((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(u3) : (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) * (unsigned)(((unsigned)((unsigned)(s2)) ^ cs)))) << ((unsigned)(1400944332u) & 31u))))) | 0u))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(1303525892u) | (unsigned)(u3))) + (unsigned)(((unsigned)(3269868554u) <= ((unsigned)(u3) ^ cs))))));
  cs = csmix(cs, (unsigned)(((unsigned)(u4) & (unsigned)(((unsigned)(((unsigned)(((unsigned)(1748276201u) >> ((unsigned)((unsigned)(s1)) & 31u))) * (unsigned)((((unsigned)(2550070277u) & 1u) ? (unsigned)(u3) : (unsigned)((unsigned)(s1)))))) * (unsigned)((~((unsigned)(u3) | 0u))))))));
  cs = csmix(cs, (unsigned)((unsigned)(s1)));
  for (unsigned g6 = 0u; g6 < 11u; g6++) {
    unsigned i5 = g6;
    cs = csmix(cs, i5);
    if ((unsigned)((unsigned)(s1)) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)(1383075737u) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(1883576536u) / ((unsigned)(3786400667u) | 1u))) + (unsigned)(((unsigned)(u4) | (unsigned)(2502547075u))))) / ((unsigned)(((unsigned)(((unsigned)(u3) % ((unsigned)(495026027u) | 1u))) >> ((unsigned)(2956704358u) & 31u))) | 1u))) & 31u))));
      cs = csmix(cs, (unsigned)(i5));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(1458513145u) != ((unsigned)((~((unsigned)(3912323483u) | 0u))) ^ cs))) << ((unsigned)((unsigned)(s2)) & 31u))) + (unsigned)(2912423487u))));
    }
    u3 = (unsigned)(2019418047u) & 0xffffffffu;
    for (unsigned g8 = 0u; g8 < 4u; g8++) {
      unsigned i7 = g8;
      cs = csmix(cs, i7);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(i7) - (unsigned)(3041951074u))) << ((unsigned)(2155583298u) & 31u))) < ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) ^ (unsigned)(1608834238u))) & (unsigned)(((unsigned)(1696860886u) / ((unsigned)(3846588993u) | 1u))))) ^ (unsigned)(((unsigned)(((unsigned)(u3) & (unsigned)(3892207465u))) % ((unsigned)(((unsigned)((unsigned)(s1)) - (unsigned)(141089314u))) | 1u))))) ^ cs))));
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s2)) - (unsigned)(785925323u))));
    }
    cs = csmix(cs, (unsigned)(i5));
  }
  cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)((unsigned)(s2)) > ((unsigned)(u4) ^ cs))) & 1u) ? (unsigned)(((unsigned)(987734701u) / ((unsigned)((unsigned)(s2)) | 1u))) : (unsigned)(((unsigned)(((unsigned)((((unsigned)(u3) & 1u) ? (unsigned)(38690203u) : (unsigned)(1421794556u))) + (unsigned)(u4))) + (unsigned)(((unsigned)(((unsigned)(u3) >> ((unsigned)(2588820319u) & 31u))) ^ (unsigned)(((unsigned)(u3) / ((unsigned)(((unsigned)(u3) ^ cs)) | 1u))))))))));

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  printf("checksum=%08x\n", cs);
  return 0;
}
