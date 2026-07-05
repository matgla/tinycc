/* Regression for fuzz longlong seed 177973: dead-store elimination must keep
 * loop-carried 64-bit stores that are read after the loop. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s1 = (short)(1387562435u & 0xffff);
  long s2 = (long)(377662383u & 0xffffffff);
  unsigned u3 = 2823225656u;
  unsigned u4 = 394813716u;
  unsigned u5 = 1400048610u;
  unsigned u6 = 1955528848u;
  unsigned u7 = 3583924159u;
  unsigned u8 = 1961749609u;
  unsigned arr9[8] = { 162282049u, 3083490368u, 3121249303u, 1529317121u, 1088838545u, 4264032598u, 4018119753u, 139667667u };
  unsigned arr10[8] = { 772633448u, 2389432743u, 3649729054u, 3786420557u, 327561605u, 1558602513u, 612551075u, 1765084814u };
  unsigned long long q11 = (((unsigned long long)(u8)) << 32) | (unsigned long long)(u3);
  unsigned long long q12 = (((unsigned long long)(u7)) << 32) | (unsigned long long)(u5);
  u6 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) - (unsigned)((unsigned)(s1)))) ^ (unsigned)((((unsigned)((unsigned)(s1)) & 1u) ? (unsigned)(u3) : (unsigned)(u4))))) == ((unsigned)(((unsigned)(((unsigned)(3983507679u) % ((unsigned)((unsigned)(s2)) | 1u))) == ((unsigned)(3874120649u) ^ cs))) ^ cs))) << ((unsigned)(((unsigned)(882899152u) - (unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(((unsigned)(arr9[((unsigned)(u5) & 7u)]) <= ((unsigned)(3121754906u) ^ cs))))))) & 31u))) & 0xffffffffu;
  { unsigned g14 = 0u;
    while (g14 < 3u) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      for (unsigned g16 = 0u; g16 < 7u; g16++) {
        unsigned i15 = g16;
        cs = csmix(cs, i15);
        cs = csmix(cs, (unsigned)(arr9[((unsigned)(i15) & 7u)]));
        cs = csmix(cs, (unsigned)(1045604263u));
      }
      if ((unsigned)((-((unsigned)(u6) | 0u))) & 1u) {
        q12 = (1092886193545822244ull) >> ((unsigned)((unsigned)(s1)) & 63u);
      } else {
        q12 = (((unsigned long long)(unsigned)((~((unsigned)(1093669475u) | 0u))))) << ((unsigned)((-((unsigned)(2871101393u) | 0u))) & 63u);
      }
      cs = csmix(cs, (unsigned)(1515081993u));
      g14++;
    }
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, (unsigned)(q11) ^ (unsigned)(q11 >> 32));
  cs = csmix(cs, (unsigned)(q12) ^ (unsigned)(q12 >> 32));
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
