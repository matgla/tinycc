/* ra:setif_fuse (setif_branch_remat) and a barrel-shift-annotated CMP.
 *
 * setif_branch_remat redoes `CMP a,b; t = SETIF` at each distant branch on t
 * and deletes the SETIF.  The copy is a new instruction with its own
 * orig_index, and barrel-shift fusion keeps a shift folded into the CMP's src2
 * (`cmp.w r4, ip, lsr #24`) in a side table keyed by orig_index: the copy
 * compared the unshifted register (`cmp.w ip, #1` after `lsl #24`).  t_low8
 * is wrong on the plain base; t_mixed (an int8 re-extension that ext_elim
 * removes) only became reachable with ext_elim. */
#include <stdint.h>
#include <stdio.h>

#define NI __attribute__((noinline))

uint32_t g[8] = {1, 0x101, 2, 0xffffff01u, 0x80, 0xff, 0x7f, 0x12345601u};
int32_t gi[8] = {0xff, 0x7f, 3, 0x101, 0x80, 0xff, 0x7f, 0x12345601};
int8_t z[8] = {-3, -2, -1, 0, 1, 2, 3, 4};

/* c <= 0xff always holds for an unsigned char, so t == ((uint8_t)g[i] == 1). */
NI int t_low8(int i, int k, int u2, int u3, unsigned char c, int b)
{
  int t = (signed char)(c <= 0xff) == (uint8_t)g[i];
  return (unsigned char)k * (t ? (signed char)b : z[i]);
}

NI int t_mixed(int i, int j, int a2, int a3, char v0, int64_t q64)
{
  int8_t v1 = (int8_t)((((char)(signed char)(((0xff) >= (v0)))) == ((uint8_t)(signed char)((uint32_t)(g[i])))));
  int s = (int32_t)((uint32_t)((char)gi[(j + 3) % 8]) *
                    (uint32_t)((((_Bool)(v1)) ? ((signed char)((int32_t)(q64 >> 32))) : (z[i]))));
  if (s & 1)
    return 7;
  return (uint16_t)(signed char)(gi[(j + 3) % 8]);
}

int main(void)
{
  for (int i = 0; i < 8; i++)
    printf(" %d", t_low8(i, 3, 0, 0, 0x80, 5));
  printf("\n");
  for (int i = 0; i < 8; i++)
  {
    for (int j = 0; j < 8; j++)
      printf(" %d", t_mixed(i, j, 0, 0, (char)(0x7e + j), (int64_t)((uint64_t)(0x11 + 2 * j) << 32)));
    printf("\n");
  }
  return 0;
}
