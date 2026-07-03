#include <stdio.h>

/*
 * Fuzz longlong seed 3161 reduction (O1-only): q13 = q10 - q12 const-folds to
 * the 64-bit value 0xFFFFFFFFC5FC7688, stored as a sign-extended IMM32
 * (#-973339512).  dead-store-elim removes q13's dead init, making it
 * single-def, so ssa:var_to_param_forward inlines the immediate into
 * `T <- #imm SHR #32`.  ssa:fold then evaluated the 64-bit SHR with 32-bit
 * arithmetic ((uint32_t)val >> 32 -> 0), losing the 0xFFFFFFFF high word in
 * (unsigned)(q13 >> 32).  Fix: fold_binary evaluates at 64-bit width when the
 * dest is 64-bit and pools the result as an I64 immediate when it does not
 * fit in int32.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(2414563703u) * (unsigned)(lr))) << ((unsigned)(lr) & 31u))) + (unsigned)(1134642834u))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(2822973280u) - (unsigned)(pa))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(1293094274u & 0xff);
  char s4 = (char)(1019606941u & 0xff);
  char s5 = (char)(895490826u & 0xff);
  unsigned u6 = 2499834214u;
  unsigned u7 = 3473173726u;
  unsigned u8 = 3193313633u;
  unsigned arr9[8] = { 3315163969u, 2955383032u, 629069259u, 2156272629u, 2191751227u, 3674675807u, 3295517244u, 619049246u };
  unsigned long long q10 = (((unsigned long long)(u8)) << 32) | (unsigned long long)(u6);
  unsigned long long q11 = (((unsigned long long)(u6)) << 32) | (unsigned long long)(u7);
  unsigned long long q12 = (((unsigned long long)(u8)) << 32) | (unsigned long long)(u7);
  unsigned long long q13 = (((unsigned long long)(u8)) << 32) | (unsigned long long)(u6);
  struct S st14 = { 1313432834u, 3077831965u, 685701110u };
  for (unsigned g16 = 0u; g16 < 6u; g16++) {
    unsigned i15 = g16;
    cs = csmix(cs, i15);
  }
  q13 = (q10) - (q12);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, (unsigned)(q10) ^ (unsigned)(q10 >> 32));
  cs = csmix(cs, (unsigned)(q11) ^ (unsigned)(q11 >> 32));
  cs = csmix(cs, (unsigned)(q12) ^ (unsigned)(q12 >> 32));
  cs = csmix(cs, (unsigned)(q13) ^ (unsigned)(q13 >> 32));
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  cs = csmix(cs, st14.f0);
  cs = csmix(cs, st14.f1);
  cs = csmix(cs, st14.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
