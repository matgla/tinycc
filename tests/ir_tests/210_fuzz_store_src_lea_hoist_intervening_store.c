/* Regression for differential-fuzz seed 2137: wrong-code at -O2 only.
 *
 * Root cause: the ARM SSA fusion ssa_gen_arm_fuse_store_src_through_add_imm
 * fuses the deref *source* of a `V <- *t_lea [STORE]` by rewriting the
 * address-computing `t_lea = ADD(base,#imm)` itself into LOAD_INDEXED — i.e.
 * the load is RELOCATED upward from the store to the ADD's definition site.
 *
 * Here arr8[u5&7] (u5 const, so index 7) is read once before the store, and the
 * fully-unrolled `for k cs=csmix(cs,arr8[k])` re-reads arr8[7] afterwards.  GVN
 * CSE'd the unrolled k=7 read's address back to the first read's LEA, so that
 * LEA's def sits *before* the intervening `arr8[u5&7]=...` store.  Hoisting the
 * load to the LEA made the k=7 iteration read the pre-store (initializer) value
 * 2135755045 instead of the stored 1328358578.
 *
 * Fix: bail the fusion when any aliasing store/call or control-flow op lies
 * between the LEA def and the store.  Ground truth (gcc -m32 -funsigned-char):
 * checksum=794b3b5f (== tcc -O0/-O1).  Buggy -O2 produced 94e4fd13.
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
  if ((unsigned)(((unsigned)(pb) + (unsigned)(((unsigned)(2730049241u) ^ (unsigned)(2515426365u))))) & 1u) lr += (unsigned)(2667049707u);
  lr = (unsigned)(((unsigned)(lr) << ((unsigned)(pa) & 31u)));
  if ((unsigned)((~((unsigned)(lr) | 0u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) << ((unsigned)(pa) & 31u))) ^ (unsigned)(1756608470u)));
  lr = (unsigned)(913368522u);
  return (unsigned)(((unsigned)(lr) % ((unsigned)(((unsigned)(((unsigned)(pb) - (unsigned)(2865314563u))) * (unsigned)(((unsigned)(lr) >= ((unsigned)(4087122637u) ^ lr))))) | 1u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)((((unsigned)((~((unsigned)(1818514488u) | 0u))) & 1u) ? (unsigned)(((unsigned)((~((unsigned)(pb) | 0u))) << ((unsigned)(lr) & 31u))) : (unsigned)(((unsigned)(1455863585u) + (unsigned)(pb)))));
  if ((unsigned)(3409206124u) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) >> ((unsigned)(pb) & 31u))) & (unsigned)((~((unsigned)(4079143888u) | 0u)))));
  lr = (unsigned)(((unsigned)(3297751734u) >> ((unsigned)(((unsigned)(lr) > ((unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(1002529969u) : (unsigned)(pa))) ^ lr))) & 31u)));
  return (unsigned)(((unsigned)(pa) % ((unsigned)(((unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(626592917u) : (unsigned)(8177024u))) * (unsigned)(pb))) | 1u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  int s3 = (int)(2110694111u & 0xffffffff);
  long s4 = (long)(111068577u & 0xffffffff);
  unsigned u5 = 3354722087u;
  unsigned u6 = 2119852703u;
  unsigned u7 = 1382020827u;
  unsigned arr8[8] = { 2813480867u, 2980235247u, 2035528196u, 68816940u, 4236818862u, 4015078902u, 367130500u, 2135755045u };

  u6 = (unsigned)((-((unsigned)(helper2(((unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) != ((unsigned)(1223808390u) ^ cs))) * (unsigned)(((unsigned)(1397752534u) - (unsigned)(u5)))), ((unsigned)((-((unsigned)(arr8[((unsigned)(u7) & 7u)]) | 0u))) % ((unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(1411512347u))) | 1u)))) | 0u))) & 0xffffffffu;
  arr8[((unsigned)(u5) & 7u)] = (unsigned)(1328358578u);
  u5 = (unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(1329077856u))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(830547453u));
  u7 = (unsigned)((-((unsigned)((-((unsigned)(1378819017u) | 0u))) | 0u))) & 0xffffffffu;

  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
