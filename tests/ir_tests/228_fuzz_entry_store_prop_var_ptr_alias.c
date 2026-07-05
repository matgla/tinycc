/* Regression for differential-fuzz ptr seeds 206/368/394: wrong-code at -O1/-O2.
 *
 * Root cause: tcc_ir_opt_entry_store_prop (ir/opt_memory.c) forwards an entry-BB
 * constant initializer store to a stack slot into a later pointer-deref load that
 * resolves to the same offset.  Its Phase-2.5 invalidator kills the forwarded
 * entry when a later store overwrites that slot — but it could only resolve the
 * store's address when the pointer was tracked through a TEMP.  An alias pointer
 * materialized into a VAR — `V = &arr[k]` lowered as `V <- Addr[StackLoc] ADD
 * #imm` — was never recorded in var_lea_map (the ADD/ASSIGN/LEA handlers were
 * TEMP-dest only), so a store `*V = ...` (or through a TEMP copied from V) did
 * NOT invalidate the entry, and the stale initializer was forwarded into the
 * later read — overwriting the value just stored through the alias.
 *
 * Here p12=&arr9[u6&7] (folds to a constant index) is a VAR pointer; the store
 * `*p12 = ...` must be seen as overwriting arr9[that index], but entry_store_prop
 * forwarded arr9's original initializer into the `*p12` read at line 50/65.
 *
 * Fix: record VAR-dest stack addresses (`Addr[StackLoc]` and `Addr[StackLoc] +
 * const`) in var_lea_map so Phase 2.5 invalidates the matching entry-store.
 * (-fno-store-load-fwd / -fno-const-prop "fix" it; entry_store_prop, gated by
 * store-load-fwd, is the pass that forwards the stale value — NOT sl_forward.)
 *
 * Ground truth (tcc -O0 == arm-none-eabi-gcc -O2): checksum=42619475.
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
  lr = (unsigned)(1412684813u);
  lr = (unsigned)(lr);
  lr = (unsigned)(lr);
  return (unsigned)(((unsigned)((((unsigned)(((unsigned)(lr) * (unsigned)(pb))) & 1u) ? (unsigned)(2639808682u) : (unsigned)((~((unsigned)(lr) | 0u))))) * (unsigned)(lr))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(1644798451u & 0xff);
  unsigned u3 = 1167847393u;
  unsigned u4 = 1235083909u;
  unsigned u5 = 3981130658u;
  unsigned u6 = 235067097u;
  unsigned u7 = 1242786835u;
  unsigned arr8[8] = { 2315773148u, 1446160379u, 4173698330u, 862168000u, 3055675156u, 1602592095u, 269567771u, 1632316400u };
  unsigned arr9[8] = { 3921834365u, 3197602340u, 3331497935u, 747910593u, 1221941933u, 2209344408u, 1078800947u, 1271869684u };
  unsigned *p10 = &u3;
  unsigned *p11 = &arr9[0u];
  unsigned *p12 = &arr9[((unsigned)(u6) & 7u)];
  unsigned *p13 = &arr9[0u];

  *p12 = (unsigned)(((unsigned)((~((unsigned)(((unsigned)(4056038537u) | (unsigned)(((unsigned)(u3) % ((unsigned)(1265873269u) | 1u))))) | 0u))) / ((unsigned)(((unsigned)(3197555072u) / ((unsigned)(((unsigned)(((unsigned)(41108428u) ^ (unsigned)(arr8[((unsigned)(1550865276u) & 7u)]))) > ((unsigned)(((unsigned)(u5) ^ (unsigned)(((unsigned)(u5) ^ cs)))) ^ cs))) | 1u))) | 1u)));
  cs = csmix(cs, *p13);
  *p13 = (unsigned)(arr9[((unsigned)(u6) & 7u)]);
  cs = csmix(cs, *p12);
  arr9[((unsigned)(u4) & 7u)] = (unsigned)((((unsigned)(((unsigned)(((unsigned)((*p13)) - (unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(arr8[((unsigned)(3865316002u) & 7u)]))))) + (unsigned)(((unsigned)(((unsigned)(u5) < ((unsigned)((*p10)) ^ cs))) % ((unsigned)((~((unsigned)(arr9[((unsigned)(3259563982u) & 7u)]) | 0u))) | 1u))))) & 1u) ? (unsigned)(((unsigned)((-((unsigned)(552639354u) | 0u))) / ((unsigned)((-((unsigned)(helper1(arr9[((unsigned)(1971886288u) & 7u)], 180131380u)) | 0u))) | 1u))) : (unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) << ((unsigned)((unsigned)(s2)) & 31u))) << ((unsigned)(helper1(u5, arr8[((unsigned)(u4) & 7u)])) & 31u))) > ((unsigned)(((unsigned)((-((unsigned)(3745895994u) | 0u))) << ((unsigned)(3709973366u) & 31u))) ^ cs)))));
  u3 = (unsigned)(((unsigned)(((unsigned)(4234717292u) / ((unsigned)(((unsigned)(867144892u) & (unsigned)((*p13)))) | 1u))) - (unsigned)((-((unsigned)((*p11)) | 0u))))) & 0xffffffffu;

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  cs = csmix(cs, *p10);
  cs = csmix(cs, *p11);
  cs = csmix(cs, *p12);
  cs = csmix(cs, *p13);
  printf("checksum=%08x\n", cs);
  return 0;
}
