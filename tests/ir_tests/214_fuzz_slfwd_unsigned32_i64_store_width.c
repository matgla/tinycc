/* Regression for differential-fuzz seed 3210: wrong-code at -O1/-O2.
 *
 * Root cause: sl_forward (ir/opt_memory.c) widens a tracked stack STORE to a
 * 64-bit (8-byte) access whenever the stored value is an I64-tagged immediate.
 * But an unsigned 32-bit constant > INT32_MAX (e.g. `st.f0 = 2681438730u`) is
 * ALSO encoded as an I64 immediate (high word 0); that store to a 32-bit field
 * still writes only 4 bytes.  Treating it as 8 bytes made the field look like a
 * 64-bit store covering the NEXT field, and the cross-offset upper-half forward
 * (FORWARD-HI) then read its bogus zero upper half as the next field's value
 * (st9.f1), collapsing a `1045526505u / (st9.f1|1)` shift count to 0 and
 * dropping the shift entirely.
 *
 * (-fno-const-prop "fixes" it only by suppressing the const-prop that exposes
 * the constant; const_var_prop / const_prop_tmp / sl_forward are sound enablers,
 * not the bug.)
 *
 * Fix: only widen an I64-immediate store to 64-bit when the value genuinely
 * needs 64 bits (upper word is neither a sign- nor a zero-extension of the low
 * word).  Ground truth (gcc -m32 -funsigned-char): checksum=a720d0d4 (== tcc
 * -O0).  Buggy -O1/-O2 produced 2c0f55a4.
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
  lr = (unsigned)(((unsigned)(((unsigned)(2768048974u) / ((unsigned)(((unsigned)(2636263917u) ^ (unsigned)(2340048872u))) | 1u))) % ((unsigned)(1694153810u) | 1u)));
  if ((unsigned)(((unsigned)(((unsigned)(3786486382u) & (unsigned)(lr))) != ((unsigned)(pa) ^ lr))) & 1u) lr += (unsigned)((((unsigned)(pa) & 1u) ? (unsigned)(((unsigned)(lr) + (unsigned)(2774132375u))) : (unsigned)((~((unsigned)(pa) | 0u)))));
  if ((unsigned)(((unsigned)(((unsigned)(1839680508u) * (unsigned)(lr))) * (unsigned)((((unsigned)(lr) & 1u) ? (unsigned)(2997907743u) : (unsigned)(lr))))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) >> ((unsigned)(1365172040u) & 31u))) / ((unsigned)((-((unsigned)(pb) | 0u))) | 1u)));
  lr = (unsigned)(3813244052u);
  return (unsigned)((-((unsigned)(pb) | 0u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(868413123u & 0xffffffff);
  int s3 = (int)(554592584u & 0xffffffff);
  unsigned u4 = 466627862u;
  unsigned u5 = 1793871214u;
  unsigned u6 = 2681438730u;
  unsigned u7 = 3706215066u;
  unsigned arr8[8] = { 1963474376u, 2774869103u, 1511594746u, 2531075123u, 1840466245u, 1697757103u, 2122951110u, 228956426u };
  struct S st9 = { 2049935365u, 3192644826u, 3254837911u };

  cs = csmix(cs, (unsigned)(helper1((-((unsigned)(3498940658u) | 0u)), ((unsigned)(arr8[((unsigned)(293693143u) & 7u)]) >> ((unsigned)(((unsigned)(u4) ^ (unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(arr8[((unsigned)(u5) & 7u)]))))) & 31u)))));
  for (unsigned g11 = 0u; g11 < 6u; g11++) {
    unsigned i10 = g11;
    cs = csmix(cs, i10);
    st9.f1 = (unsigned)(3037573u);
    arr8[((unsigned)(u4) & 7u)] = (unsigned)((unsigned)(s3));
    if ((unsigned)(((unsigned)(((unsigned)(st9.f2) + (unsigned)(3523348475u))) >> ((unsigned)(((unsigned)(st9.f1) % ((unsigned)(((unsigned)(((unsigned)(86012988u) ^ (unsigned)(u7))) & (unsigned)(((unsigned)(300063324u) >> ((unsigned)(2724840517u) & 31u))))) | 1u))) & 31u))) & 1u) {
      arr8[((unsigned)(1545188411u) & 7u)] = (unsigned)(arr8[((unsigned)(i10) & 7u)]);
      cs = csmix(cs, (unsigned)(helper1((~((unsigned)(i10) | 0u)), (((unsigned)(((unsigned)(i10) >> ((unsigned)(((unsigned)(3104413417u) >> ((unsigned)(arr8[((unsigned)(u5) & 7u)]) & 31u))) & 31u))) & 1u) ? (unsigned)(((unsigned)(helper1(u5, 2931791461u)) <= ((unsigned)(st9.f1) ^ cs))) : (unsigned)(670824026u)))));
      u5 = (unsigned)(arr8[((unsigned)(731979101u) & 7u)]) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)((~((unsigned)((unsigned)(s3)) | 0u))) | 0u))) * (unsigned)(((unsigned)(arr8[((unsigned)(u4) & 7u)]) % ((unsigned)(((unsigned)((-((unsigned)(u6) | 0u))) >= ((unsigned)(4080676290u) ^ cs))) | 1u))))));
      cs = csmix(cs, (unsigned)((-((unsigned)(((unsigned)(((unsigned)((-((unsigned)(548797213u) | 0u))) / ((unsigned)(((unsigned)(2592776831u) + (unsigned)(278909466u))) | 1u))) % ((unsigned)(((unsigned)((((unsigned)(u7) & 1u) ? (unsigned)(2260351070u) : (unsigned)(3199009968u))) * (unsigned)(helper1(arr8[((unsigned)(u7) & 7u)], st9.f2)))) | 1u))) | 0u))));
    }
    { unsigned g13 = 0u;
      while (g13 < 8u) {
        unsigned i12 = g13;
        cs = csmix(cs, i12);
        arr8[((unsigned)(u4) & 7u)] = (unsigned)(((unsigned)((((unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(((unsigned)(3223645859u) & (unsigned)(arr8[((unsigned)(i10) & 7u)]))) : (unsigned)((-((unsigned)(3416797086u) | 0u))))) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(((unsigned)((unsigned)(s2)) ^ cs)))) / ((unsigned)((((unsigned)(2517982425u) & 1u) ? (unsigned)((((unsigned)(((unsigned)(2664969218u) + (unsigned)(st9.f1))) & 1u) ? (unsigned)(((unsigned)(499226745u) & (unsigned)(st9.f0))) : (unsigned)(arr8[((unsigned)(i10) & 7u)]))) : (unsigned)(arr8[((unsigned)(u4) & 7u)]))) | 1u)));
        st9.f2 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(2966928455u) & (unsigned)(arr8[((unsigned)(u7) & 7u)]))) << ((unsigned)(((unsigned)(i10) & (unsigned)(arr8[((unsigned)(2111298494u) & 7u)]))) & 31u))) ^ (unsigned)(helper1((-((unsigned)((unsigned)(s3)) | 0u)), ((unsigned)(598929930u) & (unsigned)(i12)))))) + (unsigned)(((unsigned)(((unsigned)(3060006005u) - (unsigned)(helper1(st9.f2, arr8[((unsigned)(2617280095u) & 7u)])))) + (unsigned)(((unsigned)(i12) - (unsigned)(((unsigned)(arr8[((unsigned)(4119174716u) & 7u)]) + (unsigned)(3595704946u)))))))));
        cs = csmix(cs, (unsigned)(((unsigned)(686009685u) >> ((unsigned)(i10) & 31u))));
        g13++;
      }
    }
    st9.f0 = (unsigned)(u6);
    cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(((unsigned)(st9.f1) | (unsigned)((unsigned)(s3)))) : (unsigned)(((unsigned)((unsigned)(s3)) - (unsigned)(st9.f0))))) | (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u4) & 7u)]) - (unsigned)(u6))) << ((unsigned)(((unsigned)(1045526505u) / ((unsigned)(st9.f1) | 1u))) & 31u))))) | 0u))));
  }
  u5 = (unsigned)((((unsigned)((-((unsigned)((~((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(u6))) | 0u))) | 0u))) & 1u) ? (unsigned)(857151980u) : (unsigned)(((unsigned)((unsigned)(s3)) << ((unsigned)((-((unsigned)(u5) | 0u))) & 31u))))) & 0xffffffffu;

  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
