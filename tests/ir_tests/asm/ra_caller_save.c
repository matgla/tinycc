/* ra:caller_save: r4-r11 hold the table, the bound and eight loop invariants
 * until the loop ends, so the hot index has no callee-saved register left.  It
 * crosses one call that only some iterations make; it lives in a caller-saved
 * register saved around that call instead of in a stack slot every iteration
 * reloads. */
typedef unsigned u32;

u32 cold(u32 x);

u32 walk(const u32 *tab, u32 n, const u32 *p)
{
  u32 v0 = p[0] * 3u, v1 = p[1] ^ 0x1234u, v2 = p[2] + 77u, v3 = p[3] * 5u;
  u32 v4 = p[4] - 9u, v5 = p[5] * 11u, v6 = p[6] ^ 0xbeefu, v7 = p[7] + 1000u;
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = acc * 31u + tab[i] * (i + 1u) + ((v0 ^ i) + (v1 & i) + (v2 | i) + (v3 - i)) + ((v4 + i) ^ (v5 * i)) +
          ((v6 >> (i & 7u)) ^ (v7 << (i & 3u)));
    if (tab[i] == 0xdeadu)
      acc += cold(i + acc);
  }
  return acc;
}
