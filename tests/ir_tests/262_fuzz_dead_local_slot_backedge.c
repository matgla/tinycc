#include <stdio.h>
#include <string.h>

/*
 * Fuzz float seed 6632 reduction (O1/O2): tcc_ir_opt_dead_local_slot_elim
 * (ir/opt_memory.c) used position-only liveness (`read.pos > store.pos`)
 * in its kill loops without a loop-back-edge guard.  The in-loop store
 * `st7.f0 = ...` (StackLoc slot) sits at a later position than the loop-top
 * read `csmix(cs, st7.f0)`, but the read executes AFTER the store via the
 * back edge — the store is loop-carried-live.  Once the post-loop reads
 * were const-folded away, the only remaining read was the loop-top one and
 * the pass NOP'd the store, feeding csmix a stale f0 on every iteration.
 * Fix: when the function has any back edge, an overlapping read at ANY
 * position keeps the store (mirrors the pass's own dls_has_backedge note).
 * Expected checksum (gcc -O2 arm-none-eabi + tcc -O0/-Os): 55469fa0.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned fbits_d(double d){ unsigned u[2]; memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }
static unsigned fbits_f(float f){ unsigned u; memcpy(&u, &f, sizeof u); return u; }
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(1557119782u) | (unsigned)(((unsigned)(2135525651u) + (unsigned)(2326486212u))))) << ((unsigned)(pa) & 31u))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(1748289438u & 0xff);
  char s3 = (char)(924031655u & 0xff);
  short s4 = (short)(705457160u & 0xffff);
  unsigned u5 = 3882544056u;
  unsigned u6 = 254964306u;
  struct S st7 = { 2495963322u, 2788539358u, 185024739u };
  double f8 = 0x1.4e07f00000000p+22;
  float f9 = -0x1.9277760000000p+40f;
  { unsigned g11 = 0u;
    while (g11 < 9u) {
      unsigned i10 = g11;
      cs = csmix(cs, i10);
      cs = csmix(cs, (unsigned)(st7.f0));
      st7.f0 = (unsigned)(((unsigned)(((unsigned)(i10) * (unsigned)(((unsigned)(u6) * (unsigned)(st7.f0))))) / ((unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(((unsigned)(379381216u) % ((unsigned)(1655275644u) | 1u))))) + (unsigned)(((unsigned)(((unsigned)(4170303338u) | (unsigned)(st7.f1))) << ((unsigned)(i10) & 31u))))) | 1u)));
      g11++;
    }
  }
  st7.f0 = (unsigned)((~((unsigned)(((unsigned)(((unsigned)(u5) % ((unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)(u5))) | 1u))) * (unsigned)((-((unsigned)(((unsigned)(760951248u) ^ (unsigned)(1789646695u))) | 0u))))) | 0u)));
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  cs = csmix(cs, fbits_d(f8));
  cs = csmix(cs, fbits_f(f9));
  printf("checksum=%08x\n", cs);
  return 0;
}
