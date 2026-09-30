/* 466_alive_share_boundary_coholder.c — ra:alive_share leaves a SECOND live
 * holder on a register, and the phi-hint BOUNDARY REUSE handed that register
 * on without asking whether anyone else was still living there.
 *
 * When a phi-coalescing hint's partner expires exactly at cur->start, the
 * allocator lets cur take partner's register (sources are read before the dest
 * is written) and forces partner out of `active`, marking the register free.
 * That is only sound when partner was the register's ONLY holder: alive_share
 * deliberately parks a borrower on a register the allocator still shows as
 * owned, and here the borrower (u6's pre-branch value, live across the whole
 * if/else and read again at the join) outlived the partner.  Its register was
 * handed to cur and then marked free, so the join read a clobbered value —
 * after the else arm ran, u6 read back as the then arm's value while the then
 * arm's other assignments were untouched.
 *
 * This is the FIFTH wrong-code variant of this pass; the expire path guards it
 * with survivor_int and eviction learned to in 030e6fa7.  The same co-holder
 * rule now covers the two other unguarded sites (the single-register boundary
 * and the return/call pair boundary eviction).
 *
 * Verbatim fuzz repro: tests/fuzz combo_num seed 2412, reduced.  `float` seed
 * 3906 was the same bug.  Pre-fix: -O1/-O2/-Os give 79ec3067; gcc -O0/-O2 and
 * tcc -O0 all give da58982b.
 */
#include <stdio.h>
#include <string.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned fbits_d(double d){ unsigned u[2]; memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }
static unsigned fbits_f(float f){ unsigned u; memcpy(&u, &f, sizeof u); return u; }
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(pb) & (unsigned)(1081041013u))) < ((unsigned)(2680042698u) ^ lr))) + (unsigned)(((unsigned)(pa) | (unsigned)(1315446814u)))));
  if ((unsigned)(pa) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(3293082258u) >> ((unsigned)(lr) & 31u))) ^ (unsigned)(((unsigned)(1004440627u) / ((unsigned)(pb) | 1u)))));
  return (unsigned)(pa) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(lr) << ((unsigned)(pa) & 31u))) - (unsigned)((((unsigned)(533731999u) & 1u) ? (unsigned)(3650960205u) : (unsigned)(pa))))) >= ((unsigned)(1337666855u) ^ lr))) ^ lr;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s3 = (int)(1373052877u & 0xffffffff);
  long s4 = (long)(1885788017u & 0xffffffff);
  unsigned u5 = 3346008089u;
  unsigned u6 = 2687936074u;
  unsigned long long q7 = (((unsigned long long)(u6)) << 32) | (unsigned long long)(u5);
  unsigned long long q8 = (((unsigned long long)(u6)) << 32) | (unsigned long long)(u5);
  unsigned long long q9 = (((unsigned long long)(u5)) << 32) | (unsigned long long)(u6);
  int si10 = -20415;
  int si11 = 14839;
  int si12 = -6927;
  double f13 = -0x1.9c93100000000p+40;
  float f14 = -0x1.ce8f3e0000000p+31f;
  double f15 = 0x1.ef6c4a0000000p+23;
  double f16 = 0x1.c5a18c0000000p+32;
  u5 = (unsigned)((((unsigned)(((unsigned)(((unsigned)((((unsigned)(1335158747u) & 1u) ? (unsigned)(3818024347u) : (unsigned)(1922863576u))) >> ((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u6) ^ cs)))) & 31u))) + (unsigned)(((unsigned)(((unsigned)(2041510752u) % ((unsigned)(1706383214u) | 1u))) % ((unsigned)(((unsigned)(u5) * (unsigned)(1243632820u))) | 1u))))) & 1u) ? (unsigned)((~((unsigned)((unsigned)(s3)) | 0u))) : (unsigned)(helper1(((unsigned)(137600503u) + (unsigned)(((unsigned)(u5) - (unsigned)(((unsigned)(u5) ^ cs))))), ((unsigned)((~((unsigned)(u6) | 0u))) + (unsigned)(helper2(u6, (unsigned)(s3)))))))) & 0xffffffffu;
  cs = csmix(cs, ((q9) >= (q8)) ? 1u : 0u);
  if ((unsigned)(1758928585u) & 1u) {
    cs = csmix(cs, ((q8) == (q7)) ? 1u : 0u);
    cs = csmix(cs, (unsigned)(((si10) != (si12)) ? 1 : 0));
    q9 = (q8) & (9779368045435514120ull);
    u6 = (unsigned)(((unsigned)(u5) * (unsigned)(((unsigned)(((unsigned)(((unsigned)(614381499u) + (unsigned)(4012616354u))) | (unsigned)(((unsigned)(u6) * (unsigned)(845630637u))))) * (unsigned)(((unsigned)(u5) <= ((unsigned)(u6) ^ cs))))))) & 0xffffffffu;
    if ((unsigned)(((unsigned)((~((unsigned)(((unsigned)(u5) * (unsigned)(((unsigned)(u5) << ((unsigned)(u6) & 31u))))) | 0u))) % ((unsigned)((~((unsigned)((((unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)(1153068592u) : (unsigned)(u6))) & 1u) ? (unsigned)(u6) : (unsigned)(((unsigned)(2360612077u) + (unsigned)(614655603u))))) | 0u))) | 1u))) & 1u) {
      f13 = ((double)((unsigned)(u6))) / ((((double)(f16)) == (double)0) ? (double)1 : ((double)(f16)));
      u6 = (unsigned)((((unsigned)(((unsigned)(u5) / ((unsigned)(((unsigned)(((unsigned)(u6) >= ((unsigned)(2513126341u) ^ cs))) % ((unsigned)((((unsigned)(1267472430u) & 1u) ? (unsigned)(3836295788u) : (unsigned)(502572453u))) | 1u))) | 1u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(u6) ^ (unsigned)(((unsigned)(2639922324u) & (unsigned)(u6))))) - (unsigned)(helper2(((unsigned)(3125754381u) ^ (unsigned)(3907707548u)), (unsigned)(s3))))) : (unsigned)(((unsigned)(helper1((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)(3978502467u) : (unsigned)(u5)), (((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)(u5) : (unsigned)(u6)))) & (unsigned)(u5))))) & 0xffffffffu;
      f14 = (0x1.e15bf20000000p+34f) + ((float)((unsigned)(u5)));
      f14 = (f14 < -0x1p40f || f14 > 0x1p40f) ? (float)1 : f14;
    } else {
      cs = csmix(cs, (unsigned)(((((int)(short)(u5))) <= (9167)) ? 1 : 0));
    }
  }
  cs = csmix(cs, u6);
  cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
  cs = csmix(cs, (unsigned)(q8) ^ (unsigned)(q8 >> 32));
  cs = csmix(cs, (unsigned)(q9) ^ (unsigned)(q9 >> 32));
  cs = csmix(cs, fbits_d(f13));
  cs = csmix(cs, fbits_f(f14));
  printf("checksum=%08x\n", cs);
}
