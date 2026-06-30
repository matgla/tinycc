/* Regression for differential-fuzz ptr seed 323: wrong-code at -O2.
 *
 * Root cause: tcc_ir_opt_store_redundant (ir/opt_memory.c) tracks pending
 * stores and NOPs an earlier store when a later store hits the same (sym,off)
 * with no intervening read.  A DEREF read keeps a store alive only if the
 * pointer resolves to an exact (sym,off) via rse_resolve_temp_addr or to an
 * array base+runtime-index via rse_resolve_runtime_base.  A read through a
 * VAR-materialized pointer — `V = &arr[k]; T = V; x = *T` — resolves through a
 * VAR link, where rse_resolve_temp_addr bails (non-TEMP) and there is no runtime
 * addend, so the read was treated as "no read".  Here `*p11`/`*p13` read
 * arr8[u2&7] (==arr8[6]) through such a VAR pointer; the later constant-resolved
 * store `arr8[u2&7] = ...` then wrongly judged the arr8[6] initializer redundant
 * and dropped it, so the deref read picked up an unwritten slot.  Sibling of
 * 217 (which fixed the runtime-index DEREF miss); this adds the VAR-pointer miss.
 *
 * Fix: an lval DEREF read that resolves to neither an exact offset nor a runtime
 * base is conservatively treated as an aliasing read (flush all pending stores).
 * (-fno-redundant-store-elim "fixes" it; redundant-store-elim creates the bug.)
 *
 * Ground truth (tcc -O0 == arm-none-eabi-gcc -O2): checksum=fd5f7b33.
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
  char s1 = (char)(226538817u & 0xff);
  unsigned u2 = 1353060086u;
  unsigned u3 = 3597863965u;
  unsigned u4 = 1517066336u;
  unsigned u5 = 2092504747u;
  unsigned u6 = 3472915213u;
  unsigned u7 = 1556862767u;
  unsigned arr8[8] = { 1184800027u, 649254197u, 94796242u, 2293969448u, 2563293309u, 823980188u, 3343754750u, 551707433u };
  unsigned arr9[8] = { 4205256741u, 2369134671u, 702058035u, 3679474464u, 897619317u, 1539319154u, 43414982u, 1135246045u };
  unsigned *p10 = &u3;
  unsigned *p11 = &arr8[((unsigned)(u2) & 7u)];
  unsigned *p12 = &arr8[7u];
  unsigned *p13 = &arr8[((unsigned)(u2) & 7u)];
  struct S st14 = { 408076748u, 4144907187u, 765968864u };

  u4 = (unsigned)(arr9[((unsigned)(u5) & 7u)]) & 0xffffffffu;
  *p12 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) | (unsigned)(((unsigned)(arr9[((unsigned)(4136663728u) & 7u)]) << ((unsigned)(4011102313u) & 31u))))) + (unsigned)(((unsigned)(((unsigned)(u6) % ((unsigned)(u3) | 1u))) % ((unsigned)(((unsigned)(u6) + (unsigned)(2874473566u))) | 1u))))) - (unsigned)(((unsigned)(((unsigned)(((unsigned)((*p13)) | (unsigned)(u2))) | (unsigned)((unsigned)(s1)))) | (unsigned)(u3)))));
  cs = csmix(cs, *p12);
  cs = csmix(cs, (unsigned)(((unsigned)(st14.f2) << ((unsigned)(((unsigned)(arr8[((unsigned)(2686221692u) & 7u)]) % ((unsigned)(((unsigned)(3520915088u) / ((unsigned)(((unsigned)(u4) / ((unsigned)(st14.f2) | 1u))) | 1u))) | 1u))) & 31u))));
  arr8[((unsigned)(u2) & 7u)] = (unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(2604918541u) & 7u)]) / ((unsigned)(((unsigned)(st14.f0) % ((unsigned)(((unsigned)(558849301u) / ((unsigned)(u2) | 1u))) | 1u))) | 1u))) << ((unsigned)(((unsigned)((((unsigned)(((unsigned)(920100897u) | (unsigned)(arr8[((unsigned)(u3) & 7u)]))) & 1u) ? (unsigned)(4000001971u) : (unsigned)(((unsigned)(st14.f0) * (unsigned)(3166434405u))))) >> ((unsigned)((unsigned)(s1)) & 31u))) & 31u)));
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(((unsigned)((*p12)) % ((unsigned)(1944442523u) | 1u))) & 1u) ? (unsigned)(((unsigned)(u2) & (unsigned)(690709167u))) : (unsigned)((unsigned)(s1)))) & (unsigned)(((unsigned)((-((unsigned)((*p13)) | 0u))) * (unsigned)(4027995057u))))) * (unsigned)(u5))));
  cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(arr9[((unsigned)(u3) & 7u)]) % ((unsigned)(((unsigned)((((unsigned)(arr8[((unsigned)(1922325612u) & 7u)]) & 1u) ? (unsigned)(u2) : (unsigned)(st14.f1))) + (unsigned)((unsigned)(s1)))) | 1u))) & 1u) ? (unsigned)(u7) : (unsigned)((*p11)))));

  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  cs = csmix(cs, st14.f0);
  cs = csmix(cs, st14.f1);
  cs = csmix(cs, st14.f2);
  cs = csmix(cs, *p10);
  cs = csmix(cs, *p11);
  cs = csmix(cs, *p12);
  cs = csmix(cs, *p13);
  printf("checksum=%08x\n", cs);
  return 0;
}
