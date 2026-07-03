/* Fuzz regression: volatile seed 8310 (O2-only wrong checksum).
 *
 * Root cause: tcc_ir_opt_const_prop_tmp tracked a TEMP's folded constant
 * (T39 <-- #50368544) but never invalidated the entry when the same TEMP
 * position was redefined with a non-constant value.  TEMPs are normally
 * single-def, but loop unrolling renames at most UNROLL_MAX_RENAME=16
 * body-local temps per copy - the 17th+ temp keeps its position in every
 * unrolled iteration, becoming multi-def straight-line code.  The stale
 * constant (u6's iteration-0 update operand) was then propagated into
 * iterations 1 and 2, deleting u6's dependence on the loop-carried value.
 *
 * Fix: invalidate tmp_info[pos] on any non-constant TEMP redefinition,
 * mirroring the VAR-tracking path right below it.
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
  return (unsigned)(2757461938u) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(3558822790u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s3 = (int)(1010246074u & 0xffffffff);
  char s4 = (char)(944149857u & 0xff);
  char s5 = (char)(433647421u & 0xff);
  unsigned u6 = 2473133913u;
  unsigned u7 = 271856394u;
  volatile unsigned vv8 = 2124393641u;
  volatile unsigned vv9 = 1601599933u;
  volatile unsigned vv10 = 621116947u;
  struct S st11 = { 1433945728u, 4030063869u, 3628031769u };
  cs = csmix(cs, (unsigned)(st11.f2));
  { unsigned g13 = 0u;
    while (g13 < 3u) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      st11.f0 = (unsigned)(((unsigned)(3404803352u) >= ((unsigned)((unsigned)(s4)) ^ cs)));
      cs = csmix(cs, vv9);
      u6 = (unsigned)(((unsigned)((~((unsigned)(st11.f0) | 0u))) + (unsigned)(((unsigned)((((unsigned)((unsigned)(s3)) & 1u) ? (unsigned)(((unsigned)(i12) / ((unsigned)(2446040589u) | 1u))) : (unsigned)(331461044u))) & (unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u7) ^ (unsigned)((unsigned)(s5)))))))))) & 0xffffffffu;
      g13++;
    }
  }
  cs = csmix(cs, vv10);
  u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) ^ (unsigned)(st11.f0))) % ((unsigned)(((unsigned)(u7) & (unsigned)(1467000882u))) | 1u))) + (unsigned)(((unsigned)(((unsigned)(st11.f0) * (unsigned)(u7))) >> ((unsigned)(((unsigned)(u7) | (unsigned)(u6))) & 31u))))) | (unsigned)(771140593u))) & 0xffffffffu;
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, vv8);
  cs = csmix(cs, vv9);
  cs = csmix(cs, vv10);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, st11.f0);
  cs = csmix(cs, st11.f1);
  cs = csmix(cs, st11.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
