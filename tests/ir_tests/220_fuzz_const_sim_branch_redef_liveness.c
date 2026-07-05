/* Regression: loop_const_sim dropped a loop's residual store for a VAR that is
 * live after the loop, because its liveness check misread control flow.
 *
 * Reduced from differential-fuzz gen_c.py seed=8985 (-O2 wrong, -O0 correct).
 *
 * Pass:  tcc_ir_opt_loop_const_sim (ir/opt_loop_const_sim.c), gated by the
 *        loop-unroll knob (ZZ_loop_const_sim).
 * Bug:   the pass simulates a constant-trip loop and replaces it with residual
 *        ASSIGNs for each VAR it modified that is still live after the loop.
 *        Liveness was decided by lcs_var_used_after(), a LINEAR scan over
 *        instruction *indices* that stops at the first redefinition of the VAR
 *        ("redef kills the loop value").  Index order is not control-flow order:
 *        here the loop (`u4 = u3` each iteration) sits in the taken `if` branch,
 *        `u4` is read after the merge, and the NOT-taken `else` branch redefines
 *        `u4` at a LOWER index than that read.  The scan saw the dead `else`
 *        redefinition first, declared `u4` dead, and dropped the residual
 *        `u4 = u3` — so `u4` kept its pre-loop value and the checksum was wrong
 *        at -O1/-O2.  -O0 (and -fno-loop-unroll) were correct.
 * Fix:   only honour the "redef kills" shortcut in the straight-line prefix
 *        from the loop exit; once any branch is crossed, index order no longer
 *        tracks a single path, so a later redefinition cannot be assumed to
 *        dominate the use.
 *
 * Correct checksum is gcc -m32 -funsigned-char = 7a1176db (no char/long/pointer
 * dependency here, so native gcc agrees).  -O0/-Os were correct; bug at -O1/-O2.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
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
  int s1 = (int)(1732128797u & 0xffffffff);
  int s2 = (int)(389006631u & 0xffffffff);
  unsigned u3 = 3341739060u;
  unsigned u4 = 1629585307u;
  unsigned u5 = 3211636426u;
  unsigned u6 = 306404978u;
  struct S st7 = { 1276388357u, 3497120743u, 3943572882u };
  if ((unsigned)((~((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) == ((unsigned)(u6) ^ cs))) & (unsigned)(((unsigned)(626319797u) - (unsigned)(428233901u))))) / ((unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) & (unsigned)(st7.f0))) % ((unsigned)(((unsigned)(951470979u) | (unsigned)(st7.f1))) | 1u))) | 1u))) | 0u))) & 1u) {
    { unsigned g9 = 0u;
      while (g9 < 7u) {
        unsigned i8 = g9;
        cs = csmix(cs, i8);
        u4 = (unsigned)(u3) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(2665259834u));
        g9++;
      }
    }
    { unsigned g11 = 0u;
      while (g11 < 8u) {
        unsigned i10 = g11;
        cs = csmix(cs, i10);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(i10) < ((unsigned)(u3) ^ cs))) & (unsigned)(((unsigned)(u6) << ((unsigned)(u5) & 31u))))) | (unsigned)(u3))) ^ (unsigned)((unsigned)(s1)))));
        g11++;
      }
    }
  } else {
    u4 = (unsigned)((-((unsigned)(st7.f1) | 0u))) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(((unsigned)(u3) * (unsigned)(((unsigned)(407551735u) >> ((unsigned)(((unsigned)(((unsigned)(1805452803u) - (unsigned)(u5))) << ((unsigned)((-((unsigned)(u3) | 0u))) & 31u))) & 31u))))));
    cs = csmix(cs, (unsigned)(u6));
  }
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(u4) & 31u))) | 0u))) / ((unsigned)(((unsigned)(u6) & (unsigned)((~((unsigned)(u3) | 0u))))) | 1u))) * (unsigned)((~((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(((unsigned)((unsigned)(s2)) ^ cs)) & 31u))) | 0u))))));
  for (unsigned g13 = 0u; g13 < 5u; g13++) {
    unsigned i12 = g13;
    cs = csmix(cs, i12);
    cs = csmix(cs, (unsigned)(((unsigned)(2134961778u) ^ (unsigned)(((unsigned)((-((unsigned)(((unsigned)((unsigned)(s1)) * (unsigned)(u4))) | 0u))) * (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) / ((unsigned)(3973772244u) | 1u))) >> ((unsigned)(((unsigned)(u3) ^ (unsigned)(3890348686u))) & 31u))))))));
  }
  if ((unsigned)((unsigned)(s1)) & 1u) {
    cs = csmix(cs, (unsigned)((unsigned)(s1)));
    cs = csmix(cs, (unsigned)(u3));
  } else {
    if ((unsigned)(((unsigned)(((unsigned)(u4) - (unsigned)((unsigned)(s1)))) * (unsigned)(2611943076u))) & 1u) {
    }
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1500109492u) | (unsigned)(653891807u))) << ((unsigned)(st7.f1) & 31u))) << ((unsigned)(((unsigned)(((unsigned)(u3) / ((unsigned)((unsigned)(s1)) | 1u))) - (unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)(1365815327u))))) & 31u))) % ((unsigned)(st7.f1) | 1u))));
    { unsigned g15 = 0u;
      while (g15 < 7u) {
        unsigned i14 = g15;
        cs = csmix(cs, i14);
        cs = csmix(cs, (unsigned)(((unsigned)(1674859937u) >> ((unsigned)((~((unsigned)((unsigned)(s1)) | 0u))) & 31u))));
        cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(((unsigned)(((unsigned)(2444631704u) << ((unsigned)(240865830u) & 31u))) % ((unsigned)(1566443368u) | 1u))) * (unsigned)(((unsigned)(u6) | (unsigned)(((unsigned)(1116672032u) * (unsigned)(st7.f1))))))) & 1u) ? (unsigned)((~((unsigned)(((unsigned)(((unsigned)(u6) + (unsigned)(176359480u))) + (unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(2493602861u) : (unsigned)((unsigned)(s2)))))) | 0u))) : (unsigned)(2796901330u))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(416322694u) * (unsigned)((unsigned)(s1)))) | (unsigned)(((unsigned)((unsigned)(s2)) % ((unsigned)(st7.f1) | 1u))))) >> ((unsigned)(((unsigned)((-((unsigned)((unsigned)(s2)) | 0u))) * (unsigned)((-((unsigned)(u3) | 0u))))) & 31u))) & (unsigned)(((unsigned)(st7.f2) % ((unsigned)(((unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(u4) : (unsigned)(((unsigned)(u4) ^ cs)))) != ((unsigned)(u3) ^ cs))) | 1u))))));
      }
    }
    { unsigned g17 = 0u;
      while (g17 < 4u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u4) + (unsigned)(2858492259u))) % ((unsigned)((((unsigned)(((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u6) ^ cs)))) >> ((unsigned)(((unsigned)(u4) ^ (unsigned)(904085251u))) & 31u))) & 1u) ? (unsigned)(st7.f2) : (unsigned)((((unsigned)(((unsigned)(i16) & (unsigned)((unsigned)(s2)))) & 1u) ? (unsigned)((-((unsigned)(st7.f0) | 0u))) : (unsigned)(597640191u))))) | 1u))));
      }
    }
    for (unsigned g19 = 0u; g19 < 6u; g19++) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(st7.f1) & 1u) ? (unsigned)(((unsigned)(u4) * (unsigned)(3106727060u))) : (unsigned)(((unsigned)((unsigned)(s1)) ^ (unsigned)(((unsigned)((unsigned)(s1)) ^ cs)))))) << ((unsigned)((-((unsigned)(((unsigned)(u5) & (unsigned)(u3))) | 0u))) & 31u))) ^ (unsigned)(((unsigned)(((unsigned)(((unsigned)(st7.f1) << ((unsigned)((unsigned)(s2)) & 31u))) & (unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(4244173883u))))) + (unsigned)(((unsigned)(((unsigned)(st7.f2) / ((unsigned)((unsigned)(s2)) | 1u))) + (unsigned)(((unsigned)((unsigned)(s2)) > ((unsigned)(1516908929u) ^ cs))))))))));
      cs = csmix(cs, (unsigned)((-((unsigned)(u3) | 0u))));
    }
    if ((unsigned)(3684417595u) & 1u) {
      cs = csmix(cs, (unsigned)(614743498u));
      cs = csmix(cs, (unsigned)(((unsigned)(st7.f1) ^ (unsigned)((unsigned)(s1)))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(u6) - (unsigned)(594129159u))) | 0u))) >> ((unsigned)(((unsigned)((((unsigned)(st7.f0) & 1u) ? (unsigned)(u3) : (unsigned)(961034067u))) ^ (unsigned)((~((unsigned)(u4) | 0u))))) & 31u))) << ((unsigned)(((unsigned)(1722787685u) | (unsigned)(((unsigned)(u5) & (unsigned)(3971630630u))))) & 31u))));
    }
  }
  cs = csmix(cs, (unsigned)(((unsigned)(131816450u) - (unsigned)((unsigned)(s2)))));
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
