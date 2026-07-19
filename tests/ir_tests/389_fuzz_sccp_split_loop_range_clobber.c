/* Fuzz seed longlong:5039 (-O2): sccp_loop_clobbers_slot scanned only the
 * linear [start,end] range of the loop containing the load — but a multi-arm
 * while-loop (if/else with distinct latches) is recorded as SEVERAL
 * overlapping ranges, and the else arm's STORE_INDEXED that rewrites
 * st10.f2 sat in a sibling range the scan never visited.  sccp then folded
 * the in-loop (st10.f2 & st10.f1) load to its init-time constant.  Fix:
 * union all transitively-overlapping loop ranges before scanning. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S { unsigned f0; unsigned f1; unsigned f2; };
int main(void)
{
  unsigned cs = 0x12345678u;
  int s1 = (int)1575473869u;
  unsigned u2 = 2220579743u;
  unsigned u3 = 437440808u;
  unsigned u4 = 1186344733u;
  unsigned u5 = 1917599750u;
  unsigned long long q7 = (((unsigned long long)(u2)) << 32) | (unsigned long long)(u3);
  unsigned long long q8 = (((unsigned long long)(u3)) << 32) | (unsigned long long)(u2);
  unsigned long long q9 = (((unsigned long long)(u3)) << 32) | (unsigned long long)(u2);
  struct S st10 = { 15078953u, 3974190913u, 1466805912u };
  for (unsigned g12 = 0u; g12 < 9u; g12++) {
    cs = csmix(cs, g12);
    q7 = ((unsigned long long)(st10.f2 & st10.f1)) ^ q7;
    { unsigned g14 = 0u;
      while (g14 < 12u) {
        cs = csmix(cs, g14);
        cs = csmix(cs, (unsigned)(q9) ^ (unsigned)(q9 >> 32));
        cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
        g14++;
      }
    }
    if ((u4 * (u5 & ((st10.f2 ^ 2266691531u) | ((unsigned)s1 & 4179668525u)))) & 1u) {
      q8 = q8 | q7;
    } else {
      st10.f2 = (unsigned)s1;
      cs = csmix(cs, (unsigned)(q9) ^ (unsigned)(q9 >> 32));
      cs = csmix(cs, (q9 < (q9 ^ (unsigned long long)cs)) ? 1u : 0u);
    }
  }
  cs = csmix(cs, (q9 == q7) ? 1u : 0u);
  cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
  cs = csmix(cs, (unsigned)(q8) ^ (unsigned)(q8 >> 32));
  cs = csmix(cs, (unsigned)(q9) ^ (unsigned)(q9 >> 32));
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
