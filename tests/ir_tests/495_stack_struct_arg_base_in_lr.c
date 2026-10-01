/* A by-value struct argument copied to the outgoing stack area needs a base
   register when its source is out of LDR/LDRD immediate reach -- here rs[60],
   1200 bytes up the frame.  With every register holding a value (v1-v8 across
   the call, the scalar arguments not yet stored), the scratch for that base
   was LR, and the copy loaded its first pair into LR too: the base was
   overwritten and the last word read through the loaded value.  With the base
   out of LR, the second data register was find_call_scratch's last resort IP,
   which here still holds the argument x3.  Zig's AstGen.containerDecl passing
   a 20-byte ResultInfo split across r2-r3 and the stack segfaulted on the
   first. */
#include <stdio.h>

struct RI
{
  int a, b, c, d, e;
};

__attribute__((noinline)) int callee(int p, int q, struct RI r, int x1, int x2, int x3, int x4, int x5, int x6)
{
  return p + q * 2 + r.a * 3 + r.b * 5 + r.c * 7 + r.d * 11 + r.e * 13 + x1 * 17 + x2 * 19 + x3 * 23 + x4 * 29 +
         x5 * 31 + x6 * 37;
}

__attribute__((noinline)) void fill(struct RI *rs, int n)
{
  for (int i = 0; i < n; i++)
    rs[i] = (struct RI){i, i + 1, i + 2, i + 3, i + 4};
}

__attribute__((noinline)) int f(int n)
{
  struct RI rs[64];
  int v1 = n * 3, v2 = n + 7, v3 = n ^ 0x55, v4 = n * n, v5 = n - 9, v6 = n << 3, v7 = n | 0x100, v8 = n * 13;
  fill(rs, 64);
  int t = callee(n * 41, n * 43, rs[60], n * 47, n * 53, n * 59, n * 61, n * 67, n * 71);
  return t + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8;
}

int main(void)
{
  printf("%d\n", f(3));
  return 0;
}
