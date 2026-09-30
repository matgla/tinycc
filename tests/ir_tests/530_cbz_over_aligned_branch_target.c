/* A CBZ that spans a word-aligned branch target.
   The `if (!x) goto end` below fuses into CBZ, whose reach is 126 bytes and
   which cannot be widened once emitted.  The distance is taken from the
   rehearsal pass, which emits every branch wide; the real pass is smaller,
   except for the 2-byte pad that word-aligns a backward-branch target
   (`again`).  Narrowing the `y == 7` branch ahead of the CBZ flips the
   parity: the rehearsal found `again` aligned, the real pass needs the pad,
   and a CBZ the rehearsal measured at exactly 126 bytes landed at 128 --
   "CBZ/CBNZ target out of range: offset=128" at -O1, -O2 and -Os.  (Found in
   the Zig compiler's InternPool.getOrPutTrailingString.)  The filler below
   sizes the skipped block to that edge; keep it exactly as it is. */
#include <stdio.h>

__attribute__((noinline)) int f(int *p, int n, int x, int y)
{
  int s = 0;
  if (y == 7)
    p[40] = n * 9 + x;
  p[30] = y;
  if (!x)
    goto end;
  p[0] = p[1] + y;
  p[1] = p[2] + y;
  p[2] = p[3] + y;
  p[3] = p[4] + y;
  p[4] = p[5] + y;
  p[5] = p[6] + y;
  p[6] = p[7] + y;
  p[7] = p[8] + y;
  p[8] = p[9] + y;
  p[9] = p[10] + y;
  p[10] = p[11] + y;
  p[11] = p[12] + y;
  p[12] = p[13] + y;
  p[13] = p[14] + y;
  p[14] = p[15] + y;
  p[15] = p[16] + y;
  p[16] = p[17] + y;
  p[17] = p[18] + y;
  p[18] = p[19] + y;
  s += y;
again:
  s += p[s & 7];
end:
  s += 1;
  if (s < n)
    goto again;
  return s;
}

int main(void)
{
  static const int cases[][3] = {{0, 50, 7}, {1, 50, 7}, {0, 200, 3}, {5, 300, 2}, {1, 1, 9}};
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++)
  {
    int p[48];
    for (int i = 0; i < 48; i++)
      p[i] = i * 3 + 1;
    int r = f(p, cases[c][1], cases[c][0], cases[c][2]);
    unsigned sum = 0;
    for (int i = 0; i < 48; i++)
      sum = sum * 31u + (unsigned)p[i];
    printf("x=%d n=%d y=%d -> %d sum=%u\n", cases[c][0], cases[c][1], cases[c][2], r, sum);
  }
  return 0;
}
