/*
 * ptr fuzz seed 58108 reduction (O1): SCCP's entry-block store-forwarding
 * exemption (sccp_resolved_stack_write_between) ignored a conditional plain
 * STORE through a pointer held in a named VAR (*p10 = v, p10 = &arr8[u7&7]).
 * The store's target doesn't LEA-resolve (the pointer lives in a VAR, not a
 * TEMP chain), so the permissive entry-block scan skipped it and the post-
 * branch arr8[5] load folded back to the array initializer.
 * Fixed by treating unresolved escaping plain STOREs as clobbers in the
 * entry-block scan (ir/opt/ssa_opt_sccp.c); kill-switch TCC_DISABLE_PASS=ssa:sccp.
 * Ground truth (tcc -O0 == gcc -O2): checksum=a3cf844c.
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
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(2580886852u) * (unsigned)(3726958308u))) / ((unsigned)((-((unsigned)(400919403u) | 0u))) | 1u))) | (unsigned)(((unsigned)(((unsigned)(pa) / ((unsigned)(4018211533u) | 1u))) << ((unsigned)(((unsigned)(pb) ^ (unsigned)(lr))) & 31u))))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((((unsigned)(((unsigned)(((unsigned)(pb) ^ (unsigned)(lr))) % ((unsigned)(((unsigned)(1340049545u) >> ((unsigned)(374146832u) & 31u))) | 1u))) & 1u) ? (unsigned)(((unsigned)(pb) << ((unsigned)(((unsigned)(pa) << ((unsigned)(3708452229u) & 31u))) & 31u))) : (unsigned)(((unsigned)(pb) >> ((unsigned)(helper1(713053948u, pa)) & 31u))))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)(177388741u & 0xffff);
  short s4 = (short)(1595353980u & 0xffff);
  unsigned u5 = 2446213114u;
  unsigned u7 = 3181654245u;
  unsigned arr8[8] = { 2761565567u, 1556385664u, 958663198u, 3135045221u, 1246024446u, 2982652432u, 2225634060u, 2323683756u };
  unsigned *p10 = &arr8[((unsigned)(u7) & 7u)];
  unsigned *p11 = &arr8[0u];
  struct S st13 = { 1807067576u, 1030861038u, 1955007084u };

  if ((unsigned)(((unsigned)((~((unsigned)((-((unsigned)(((unsigned)(st13.f1) - (unsigned)(3094642546u))) | 0u))) | 0u))) & (unsigned)(((unsigned)(((unsigned)(((unsigned)(4256253101u) * (unsigned)((unsigned)(s3)))) << ((unsigned)(((unsigned)(arr8[((unsigned)(210853218u) & 7u)]) ^ (unsigned)((*p10)))) & 31u))) + (unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(((unsigned)(u7) + (unsigned)(st13.f1))))))))) & 1u) {
    if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2670061131u) <= ((unsigned)(((unsigned)(u5) >> ((unsigned)(2676456506u) & 31u))) ^ cs))) << ((unsigned)((*p11)) & 31u))) % ((unsigned)(((unsigned)(st13.f0) | (unsigned)(2445462090u))) | 1u))) & 1u) {
    } else {
      *p10 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(st13.f0) | 0u))) - (unsigned)((((unsigned)(3265830381u) & 1u) ? (unsigned)(1156573538u) : (unsigned)(3686535538u))))) + (unsigned)(4061881362u))) & (unsigned)((unsigned)(s3))));
    }
    cs = csmix(cs, (unsigned)(((unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(((unsigned)(3726303642u) * (unsigned)(((unsigned)(u5) - (unsigned)(1321391796u))))) : (unsigned)(((unsigned)(u5) - (unsigned)(arr8[((unsigned)(u5) & 7u)]))))) - (unsigned)(helper2((((unsigned)(3643677865u) & 1u) ? (unsigned)((~((unsigned)(4063719882u) | 0u))) : (unsigned)((*p10))), ((unsigned)(((unsigned)((unsigned)(s4)) << ((unsigned)(3325261609u) & 31u))) ^ (unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(1840434076u) : (unsigned)(arr8[((unsigned)(695208277u) & 7u)])))))))));
  }

  printf("checksum=%08x\n", cs);
  return 0;
}
