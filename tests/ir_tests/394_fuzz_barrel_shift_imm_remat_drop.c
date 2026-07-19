/* Fuzz seed longlong:6393 (-O1/-O2 wrong code).  A folded constant `u5 == 1`
 * (T165 <- #1) is XOR-src2 of `y ^ (u5 >> 31)`; barrel_shift_fusion rewrote
 * the shift into an `LSR #31` annotation on the XOR's src2 operand.  When the
 * LLONG-pair coalescer spilled the constant, ra_mark_rematerializable let
 * machine_op substitute the immediate #1 straight into the EOR
 * (`eor r,r,#1`) — the immediate ALU form has no shift field, so the `LSR #31`
 * was silently dropped (`#1 LSR #31` should be `#0`).  Fix: thumb_emit_data_
 * processing_mop32 folds the barrel shift into an immediate src2 before the
 * immediate path runs (arm-thumb-gen.c).  Correct = gcc -O0/-O1/-O2 = tcc -O0. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(1649887290u) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(57147443u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(669587647u & 0xff);
  unsigned u4 = 2250586782u;
  unsigned u5 = 18228501u;
  unsigned long long q6 = (((unsigned long long)(u5)) << 32) | (unsigned long long)(u4);
  unsigned long long q7 = (((unsigned long long)(u4)) << 32) | (unsigned long long)(u5);
  struct S st8 = { 2211343800u, 3753645911u, 1188601011u };
  struct S st9 = { 3941999885u, 3656064706u, 1994141621u };
  u5 = (unsigned)(((unsigned)(3666361184u) / ((unsigned)(u4) | 1u))) & 0xffffffffu;
  cs = csmix(cs, ((q6) == (((q6) ^ (unsigned long long)(cs)))) ? 1u : 0u);
  for (unsigned g11 = 0u; g11 < 5u; g11++) {
    unsigned i10 = g11;
    cs = csmix(cs, i10);
    if ((unsigned)((unsigned)(s3)) & 1u) {
      cs = csmix(cs, (unsigned)(2138809375u));
      cs = csmix(cs, (unsigned)(u5));
      q7 = (q7) % ((q6) | 1ull);
      cs = csmix(cs, (unsigned)(((unsigned)(3892934223u) + (unsigned)(((unsigned)(u5) << ((unsigned)(1703617442u) & 31u))))));
      cs = csmix(cs, (unsigned)(((unsigned)(st8.f2) >> ((unsigned)(3556893478u) & 31u))));
      cs = csmix(cs, (unsigned)(q6) ^ (unsigned)(q6 >> 32));
      cs = csmix(cs, (unsigned)(u4));
    }
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) >> ((unsigned)((unsigned)(s3)) & 31u))) ^ (unsigned)(((unsigned)(st8.f0) - (unsigned)(((unsigned)(st8.f0) ^ cs)))))) + (unsigned)(((unsigned)((-((unsigned)(u4) | 0u))) ^ (unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(2836422990u) : (unsigned)(4185238034u))))))) % ((unsigned)(3316389834u) | 1u))));
  }
  cs = csmix(cs, ((q6) == (((q6) ^ (unsigned long long)(cs)))) ? 1u : 0u);
  u5 = (unsigned)(((unsigned)(u4) != ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) * (unsigned)(st9.f1))) / ((unsigned)((~((unsigned)(u5) | 0u))) | 1u))) != ((unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(((unsigned)(u4) % ((unsigned)((unsigned)(s3)) | 1u))))) ^ cs))) ^ cs))) & 0xffffffffu;
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, (unsigned)(q6) ^ (unsigned)(q6 >> 32));
  cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
