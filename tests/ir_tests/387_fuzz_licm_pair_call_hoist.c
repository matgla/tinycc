/* Fuzz seed float:118 (-O2): ssa:licm hoisted the loop-invariant soft-double
 * division (__aeabi_ddiv, PAIR result in r0:r1) to the preheader, but only
 * the LOW half of the result pair got a callee-saved register (mov sl, r0);
 * the loop tail's phi copy then read the high half from r1, long since
 * clobbered by the inner loop's __aeabi_d2f/cfcmpeq calls.  Fix: the pure-call
 * hoist bails on pair-returning (64-bit) results until regalloc models a
 * hoisted pair's live range across in-loop calls. */
#include <stdio.h>
#include <string.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned fbits_d(double d){ unsigned u[2]; memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u7 = 2490833503u;
  double f11 = 0x1.0dbd240000000p+20;
  float f13 = -0x1.0489600000000p+39f;
  { unsigned g15 = 0u;
    while (g15 < 9u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      { unsigned g17 = 0u;
        while (g17 < 6u) {
          unsigned i16 = g17;
          cs = csmix(cs, i16);
          cs = csmix(cs, (((float)(f13)) == ((float)(f11))) ? 1u : 0u);
          g17++;
        }
      }
      f11 = ((double)((unsigned)(u7))) / ((((double)(f13)) == (double)0) ? (double)1 : ((double)(f13)));
      g15++;
    }
  }
  cs = csmix(cs, fbits_d(f11));
  printf("checksum=%08x\n", cs);
  return 0;
}
