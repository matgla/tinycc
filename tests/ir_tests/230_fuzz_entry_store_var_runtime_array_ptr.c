/* Regression for differential-fuzz ptr seed 3343: wrong-code at -O1/-O2.
 *
 * Root cause: tcc_ir_opt_entry_store_prop (ir/opt_memory.c) forwards a stack
 * array's entry-BB initializer into a later constant-index load of that element.
 * Phase 2.6 invalidates an array's initializers when it sees a RUNTIME-indexed
 * store into the array — but it only tracked runtime array bases held in TEMPs
 * (rt_base).  Here the alias pointer `p11 = &arr9[u6&7]` is materialised into a
 * VAR (`V = Addr[StackLoc[arr9]] + (u6&7)<<2`, runtime index), and the store
 * `*p11 = ...` goes through a TEMP copied from that VAR.  The runtime base was
 * lost across the VAR, so Phase 2.6 never fired and `arr9[2]` (= arr9[u5&7], a
 * constant index) kept forwarding its stale initializer even though `*p11`
 * overwrote it — producing a wrong value inside the loop on every iteration
 * after the first.
 *
 * Fix: track runtime array bases held in VARs (var_rt_base), propagate them
 * across VAR<->TEMP copies, and let Phase 2.6 invalidate on a runtime store
 * through a VAR pointer (or a TEMP copied from one).
 * (-fno-const-prop / -fno-store-load-fwd / TCC_DISABLE_PASS=entry_store also
 * avoid it; the real pass is entry_store via the missing VAR runtime base.)
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
int main(void)
{
  unsigned cs = 0x12345678u;
  short s1 = (short)(2066325983u & 0xffff);
  short s2 = (short)(194424264u & 0xffff);
  short s3 = (short)(94399021u & 0xffff);
  unsigned u4 = 3180927849u;
  unsigned u5 = 3746276850u;
  unsigned u6 = 1495843466u;
  unsigned u7 = 2989879508u;
  unsigned u8 = 959723001u;
  unsigned arr9[8] = { 1289056770u, 3168230936u, 4184429460u, 2374475224u, 1785023652u, 2303726943u, 1234674646u, 1062018008u };
  unsigned *p10 = &u7;
  unsigned *p11 = &arr9[((unsigned)(u6) & 7u)];
  { unsigned g13 = 0u;
    while (g13 < 6u) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      { unsigned g15 = 0u;
        while (g15 < 9u) {
          unsigned i14 = g15;
          cs = csmix(cs, i14);
          cs = csmix(cs, *p10);
          cs = csmix(cs, *p10);
          *p10 = (unsigned)(((unsigned)(((unsigned)(u4) & (unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(i14) & 7u)]) & (unsigned)((unsigned)(s2)))) << ((unsigned)(u7) & 31u))))) % ((unsigned)((((unsigned)((~((unsigned)(((unsigned)(arr9[((unsigned)(i14) & 7u)]) & (unsigned)(i12))) | 0u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(u5) & 7u)]) & (unsigned)(3035566349u))) / ((unsigned)(((unsigned)((*p10)) >> ((unsigned)(u5) & 31u))) | 1u))) : (unsigned)(i14))) | 1u)));
          cs = csmix(cs, *p11);
          g15++;
        }
      }
      for (unsigned g17 = 0u; g17 < 7u; g17++) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        *p11 = (unsigned)(arr9[((unsigned)(u7) & 7u)]);
        cs = csmix(cs, *p10);
        cs = csmix(cs, *p11);
      }
      g13++;
    }
  }
  cs = csmix(cs, (unsigned)(u5));
  u6 = (unsigned)(u7) & 0xffffffffu;
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  cs = csmix(cs, *p10);
  cs = csmix(cs, *p11);
  printf("checksum=%08x\n", cs);
  return 0;
}
