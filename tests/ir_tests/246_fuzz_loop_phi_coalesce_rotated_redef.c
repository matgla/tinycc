/* Regression: linear-scan wrongly coalesced a loop-carried phi copy across a
 * nested ROTATED loop, conflating two distinct values of the carried variable.
 *
 * From longlong fuzz seed 218 (reduced).  tcc -O0/-O1 agreed with gcc; tcc -O2
 * diverged — sole culprit knob loop-rotation (the corruption only surfaces once
 * the inner loop is bottom-tested).
 *
 * Root cause: ra_safe_loop_phi_coalesce (ir/regalloc.c) overrides the normal
 * interference check for the loop-carried pattern `cur <- partner` (def) +
 * `partner <- cur` (back-edge copy), on the reasoning that after cur's def the
 * shared register holds cur and the back-edge copy becomes an elided mov R,R.
 * That reasoning assumes cur is defined ONCE.  Here def_pos is a copy
 * `cur <- partner` at the top of the OUTER loop body, and cur (the g12-carried
 * hash) is then RE-ASSIGNED inside the rotated inner g16 loop before the outer
 * back-edge copy.  The purely-textual scan cannot model the inner back-edge, so
 * it saw only the single back-edge copy and green-lit the coalesce — sharing one
 * register for two live values and corrupting the carried checksum.
 *
 * Fix: reject the coalesce when cur is redefined anywhere between its def and the
 * back-edge copy (being more conservative in coalescing is always correct).
 *
 * The bug is register-pressure sensitive: removing almost any statement makes it
 * vanish, so this stays close to the reduced fuzz seed.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)((((unsigned)(((unsigned)(3004034967u) % ((unsigned)(1292432724u) | 1u))) & 1u) ? (unsigned)(pa) : (unsigned)(((unsigned)(((unsigned)(pb) * (unsigned)(((unsigned)(pb) ^ lr)))) - (unsigned)(1281667834u)))));
  return (unsigned)(3373557484u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(557388081u & 0xff);
  int s4 = (int)(1883238907u & 0xffffffff);
  unsigned u5 = 2437998083u;
  unsigned u6 = 1691300286u;
  unsigned u7 = 2232708693u;
  unsigned long long q8 = (((unsigned long long)(u6)) << 32) | (unsigned long long)(u7);
  unsigned long long q9 = (((unsigned long long)(u5)) << 32) | (unsigned long long)(u6);
  struct S st10 = { 2076014163u, 3422909015u, 372748145u };
  { unsigned g12 = 0u;
    while (g12 < 6u) {
      unsigned i11 = g12;
      cs = csmix(cs, i11);
      { unsigned g14 = 0u;
        while (g14 < 10u) {
          unsigned i13 = g14;
          cs = csmix(cs, i13);
          q8 = (q8) * (14558064769416727172ull);
          cs = csmix(cs, (unsigned)(q9) ^ (unsigned)(q9 >> 32));
          u5 = (unsigned)(((unsigned)(((unsigned)(i13) | (unsigned)(((unsigned)(helper1(1380047972u, st10.f1)) - (unsigned)(st10.f1))))) << ((unsigned)(((unsigned)((unsigned)(s3)) % ((unsigned)((~((unsigned)((-((unsigned)(1554868434u) | 0u))) | 0u))) | 1u))) & 31u))) & 0xffffffffu;
          g14++;
        }
      }
      for (unsigned g16 = 0u; g16 < 5u; g16++) {
        unsigned i15 = g16;
        cs = csmix(cs, i15);
        cs = csmix(cs, (unsigned)(q8) ^ (unsigned)(q8 >> 32));
        u7 = (unsigned)((unsigned)(s4)) & 0xffffffffu;
        cs = csmix(cs, ((q8) <= (q9)) ? 1u : 0u);
      }
      g12++;
    }
  }
  cs = csmix(cs, (unsigned)(q8) ^ (unsigned)(q8 >> 32));
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  printf("checksum=%08x\n", cs);
  return 0;
}
