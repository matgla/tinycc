/* Fuzz seed signed:8890 (-O1; also struct_byval:6988, longlong:6393): the
 * FLAT CMP-fold sites (cpt_try_cmp_setif_fold in const_prop_tmp.c, the two
 * value_tracking CMP+JUMPIF/SETIF folds, rot_guard_provably_folds) evaluated
 * constant compares on the RAW src2 value, ignoring a fused barrel-shift
 * annotation — the (int)(short) narrowing compare `(short)u8 < (short)u6`
 * fuses SHL16/ASR16 into the CMP.  Same bug as test 386 fixed in branch.c;
 * all sites now share evaluate_compare_condition_cmp_annotated. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(3266059524u) ^ (unsigned)(((unsigned)(pb) % ((unsigned)(pa) | 1u))))) - (unsigned)(pa))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(300010458u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s3 = (int)(1378460474u & 0xffffffff);
  int s4 = (int)(1310329711u & 0xffffffff);
  int s5 = (int)(269151517u & 0xffffffff);
  unsigned u6 = 1192372866u;
  unsigned u7 = 910097592u;
  unsigned u8 = 4059443288u;
  int si9 = -5769;
  int si10 = -12481;
  int si11 = -25689;
  int si12 = 4217;
  struct S st13 = { 3211432719u, 658855046u, 2788163002u };
  struct S st14 = { 551460579u, 3924275781u, 774662562u };
  if ((unsigned)(2339779617u) & 1u) {
    if ((unsigned)(((unsigned)(((unsigned)((unsigned)(s5)) | (unsigned)(((unsigned)(((unsigned)(2839986364u) % ((unsigned)(u7) | 1u))) + (unsigned)(((unsigned)(u7) ^ (unsigned)(st14.f1))))))) ^ (unsigned)(st14.f1))) & 1u) {
      cs = csmix(cs, (unsigned)(((-29797) == (si10)) ? 1 : 0));
    } else {
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(3203324978u) & (unsigned)(((unsigned)(4288803591u) / ((unsigned)((~((unsigned)(2855083866u) | 0u))) | 1u))))) | (unsigned)((unsigned)(s5)))));
      cs = csmix(cs, (unsigned)(((unsigned)(173699562u) % ((unsigned)(helper2((-((unsigned)(1306994960u) | 0u)), ((unsigned)((~((unsigned)(u8) | 0u))) ^ (unsigned)(((unsigned)(u6) ^ (unsigned)(1059781141u)))))) | 1u))));
    }
    cs = csmix(cs, (unsigned)(((((int)(short)(u8))) < (((int)(short)(u6)))) ? 1 : 0));
  }
  cs = csmix(cs, (unsigned)(127051753u));
  u8 = (unsigned)(u7) & 0xffffffffu;
  if ((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)(((unsigned)(helper2(3489311278u, (unsigned)(s4))) | (unsigned)((~((unsigned)(u8) | 0u))))))) << ((unsigned)((~((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(st13.f2) + (unsigned)(u7))))) | 0u))) & 31u))) & 1u) {
    u6 = (unsigned)(((unsigned)(((unsigned)(4283325332u) + (unsigned)(1100631088u))) % ((unsigned)((((unsigned)(((unsigned)(st14.f2) & (unsigned)(u6))) & 1u) ? (unsigned)(u8) : (unsigned)(61414787u))) | 1u))) & 0xffffffffu;
    if ((unsigned)(u7) & 1u) {
      cs = csmix(cs, (unsigned)(((-30134) <= (-12447)) ? 1 : 0));
      cs = csmix(cs, (unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)(st14.f0))));
    }
  }
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, (unsigned)(si9));
  cs = csmix(cs, (unsigned)(si10));
  cs = csmix(cs, (unsigned)(si11));
  cs = csmix(cs, (unsigned)(si12));
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, st13.f0);
  cs = csmix(cs, st13.f1);
  cs = csmix(cs, st13.f2);
  cs = csmix(cs, st14.f0);
  cs = csmix(cs, st14.f1);
  cs = csmix(cs, st14.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
