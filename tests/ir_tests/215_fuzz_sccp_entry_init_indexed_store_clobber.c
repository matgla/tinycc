/* Regression: differential-fuzz seed 3691 (-O1/-O2 miscompile).
 *
 * Pass: ssa_opt_sccp (ir/opt/ssa_opt_sccp.c), entry-block stack-load forwarding.
 * Root cause: a local array `arr8` is aggregate-initialized in the entry block,
 *   then conditionally overwritten via a STORE_INDEXED (`arr8[u6 & 7] = ...`)
 *   whose index is still a TEMP at SCCP time.  When SCCP resolves a later
 *   constant-index LOAD of arr8[0] it walks the dominator tree back to the
 *   entry-block initializer and applies sccp_resolved_stack_write_between() to
 *   check for an intervening clobber.  That (deliberately permissive) check
 *   only honored stores whose concrete stack offset resolved; for the
 *   STORE_INDEXED with a non-immediate index sccp_store_target_off() returned
 *   INT_MIN, so the check skipped it and SCCP forwarded the initializer
 *   (2591651399u) into the read instead of the stored 297974678u.  The final
 *   checksum over arr8 then diverged from -O0.
 * Fix: in sccp_resolved_stack_write_between() treat an unresolved-offset
 *   STORE_INDEXED / STORE_POSTINC as a clobber when the destination array's
 *   plausible stack extent covers the load slot (mirrors the indexed-base
 *   extent check sccp_no_aliasing_between() already applies on the non-entry
 *   path).
 *
 * UB-free; gcc -m32 -funsigned-char prints checksum=be32b4b4 at -O0/-O1/-O2.
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
  lr = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(732698148u) - (unsigned)(3074955496u))) | 0u))) | (unsigned)(lr)));
  lr = (unsigned)(((unsigned)(lr) & (unsigned)(((unsigned)(((unsigned)(pb) / ((unsigned)(pa) | 1u))) ^ (unsigned)(pb)))));
  return (unsigned)(1990253614u) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(2899752578u) & 1u) lr += (unsigned)((((unsigned)(((unsigned)(69647634u) - (unsigned)(lr))) & 1u) ? (unsigned)((~((unsigned)(pa) | 0u))) : (unsigned)(((unsigned)(757072977u) >> ((unsigned)(1031587619u) & 31u)))));
  lr = (unsigned)(((unsigned)(lr) & (unsigned)(((unsigned)((((unsigned)(1095473249u) & 1u) ? (unsigned)(pa) : (unsigned)(3249840823u))) < ((unsigned)(((unsigned)(2858630353u) % ((unsigned)(pb) | 1u))) ^ lr)))));
  lr = (unsigned)((-((unsigned)(((unsigned)(pb) >> ((unsigned)(helper1(955232820u, lr)) & 31u))) | 0u)));
  lr = (unsigned)(((unsigned)(((unsigned)(1817638653u) + (unsigned)(((unsigned)(lr) != ((unsigned)(3130823382u) ^ lr))))) << ((unsigned)(((unsigned)(1531896664u) + (unsigned)(pa))) & 31u)));
  if ((unsigned)(((unsigned)(((unsigned)(904253976u) == ((unsigned)(pb) ^ lr))) > ((unsigned)(((unsigned)(lr) + (unsigned)(pa))) ^ lr))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) / ((unsigned)(3370965791u) | 1u))) > ((unsigned)(64289060u) ^ lr)));
  return (unsigned)(lr) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(1389379830u & 0xff);
  long s4 = (long)(1275079846u & 0xffffffff);
  long s5 = (long)(1072615661u & 0xffffffff);
  unsigned u6 = 1892171176u;
  unsigned u7 = 1694714836u;
  unsigned arr8[8] = { 2591651399u, 2560456398u, 1198194692u, 828282125u, 2700226628u, 2040526809u, 2194617147u, 670231442u };
  struct S st9 = { 189772143u, 1592959719u, 2654261059u };
  struct S st10 = { 794945739u, 1154146354u, 1994042618u };

  arr8[((unsigned)(3407368988u) & 7u)] = (unsigned)(3680244574u);
  if ((unsigned)(((unsigned)(((unsigned)(1622371021u) < ((unsigned)(1909819360u) ^ cs))) & (unsigned)(st9.f2))) & 1u) {
    if ((unsigned)((unsigned)(s5)) & 1u) {
      arr8[((unsigned)(u6) & 7u)] = (unsigned)(297974678u);
      cs = csmix(cs, (unsigned)((~((unsigned)(u6) | 0u))));
      u7 = (unsigned)(((unsigned)(((unsigned)(st10.f1) + (unsigned)(1221786961u))) & (unsigned)(st9.f1))) & 0xffffffffu;
    }
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(st9.f0) & 1u) ? (unsigned)(((unsigned)(45776847u) & (unsigned)(2844679u))) : (unsigned)(u7))) | (unsigned)(((unsigned)(helper1(arr8[((unsigned)(3510107643u) & 7u)], u7)) * (unsigned)(1139668117u))))) ^ (unsigned)(((unsigned)(1058795697u) | (unsigned)(u7))))));
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1766971865u) < ((unsigned)(st10.f2) ^ cs))) >> ((unsigned)((-((unsigned)((unsigned)(s3)) | 0u))) & 31u))) <= ((unsigned)(551127108u) ^ cs))) << ((unsigned)(((unsigned)(((unsigned)((-((unsigned)((unsigned)(s4)) | 0u))) - (unsigned)(((unsigned)(st10.f2) / ((unsigned)(st10.f1) | 1u))))) + (unsigned)(st10.f0))) & 31u))));
  }
  u6 = (unsigned)(u7) & 0xffffffffu;
  u7 = (unsigned)(((unsigned)(st10.f2) * (unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(arr8[((unsigned)(273128472u) & 7u)]) : (unsigned)((unsigned)(s5)))))) & 0xffffffffu;

  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
