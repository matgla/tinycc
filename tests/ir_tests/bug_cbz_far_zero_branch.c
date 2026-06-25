/* Regression: the CBZ/CBNZ peephole fused `CMP rN,#0; JUMPIF EQ/NE` into a
 * single forward-only 16-bit CBZ/CBNZ (0..126 byte range) based on a distance
 * ESTIMATE, then committed the 2-byte encoding irrevocably.  When the real
 * forward distance exceeded 126 bytes, the backpatch aborted with
 * "compiler_error: CBZ/CBNZ target out of range".
 *
 * `if (x == 0) use(<sum of eight 64-bit globals>);` makes the body (literal-pool
 * loads + 64-bit adds + a call) exceed 126 bytes, so the forward zero-branch
 * overflows the CBZ range.  Fixed by disabling the unsound CBZ peephole; the
 * branch falls back to the always-correct CMP rN,#0 + B<cond>.W.
 */
#include <stdio.h>

static long long g_sink;
static long long ga = 1, gb = 2, gc = 3, gd = 4, ge = 5, gf = 6, gg = 7, gh = 8;

static void use(long long v) { g_sink += v; }

static int body(int x)
{
  if (x == 0)
    use(ga + gb + gc + gd + ge + gf + gg + gh);
  return x + 1;
}

int main(void)
{
  int r = body(0) + body(1) + body(0) + body(2);   /* 1 + 2 + 1 + 3 = 7 */
  printf("r=%d sink=%lld\n", r, g_sink);            /* sink = 2 * 36 = 72 */
  return (r == 7 && g_sink == 72) ? 0 : 1;
}
