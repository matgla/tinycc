/* Guard for forward-branch narrowing (arm-thumb-gen.c can_narrow_forward_branch).
 *
 * A forward branch's target is not emitted yet when the branch is encoded, so
 * the width is decided from the rehearsal pass's address map.  That is only
 * sound because the real pass is never larger than the rehearsal AND no literal
 * pool flush can land between the branch and its target — a flush inserts up to
 * ~1 KB and would push a committed 16-bit branch out of its +-254 byte range,
 * which th_patch_call cannot repair in place (it errors out).
 *
 * These cases stress exactly that boundary:
 *   near_*      : short forward branches that must narrow and still land right.
 *   pool_heavy  : many distinct 32-bit constants force repeated literal pool
 *                 flushes while forward branches are live across them.
 *   spread_*    : branch distances swept around the T1 range so at least one
 *                 sits near the limit in either direction.
 *   deep_nest   : nested/chained conditionals, i.e. many overlapping forward
 *                 branch ranges at once.
 * The assertions check computed values, not encodings: a mis-sized branch shows
 * up as a wrong answer or a hang, and a mis-ranged one fails the build.
 */
#include <stdio.h>

volatile int sink;

__attribute__((noinline)) int near_chain(int a, int b)
{
  int r = 0;
  if (a > 0)
    r += 1;
  if (b > 0)
    r += 2;
  if (a > b)
    r += 4;
  if (a == b)
    r += 8;
  return r;
}

/* Each arm materialises a distinct wide constant, so the literal pool fills and
 * flushes repeatedly while the if/else forward branches are outstanding. */
__attribute__((noinline)) unsigned pool_heavy(int sel)
{
  unsigned acc = 0;
  if (sel == 0) acc += 0x12345678u; else acc += 0x9abcdef0u;
  if (sel == 1) acc += 0x0f1e2d3cu; else acc += 0x4b5a6978u;
  if (sel == 2) acc += 0xdeadbeefu; else acc += 0xfeedfaceu;
  if (sel == 3) acc += 0x01234567u; else acc += 0x89abcdefu;
  if (sel == 4) acc += 0x55aa55aau; else acc += 0xaa55aa55u;
  if (sel == 5) acc += 0x13579bdfu; else acc += 0x2468ace0u;
  if (sel == 6) acc += 0xcafebabeu; else acc += 0xbaddcafeu;
  if (sel == 7) acc += 0x0badf00du; else acc += 0xf00dbabeu;
  return acc;
}

/* Forces branch distances to sweep through the T1 limit: the skipped body grows
 * with each successive guard. */
__attribute__((noinline)) int spread(int n)
{
  int r = 0;
  if (n & 1)
  {
    r += 1; sink = r; r += 2; sink = r;
  }
  if (n & 2)
  {
    r += 4; sink = r; r += 8; sink = r; r += 16; sink = r;
    r += 32; sink = r; r += 64; sink = r; r += 128; sink = r;
  }
  if (n & 4)
  {
    r += 256; sink = r; r += 512; sink = r; r += 1024; sink = r;
    r += 2048; sink = r; r += 4096; sink = r; r += 8192; sink = r;
    r += 16384; sink = r; r += 32768; sink = r; r += 65536; sink = r;
    r += 131072; sink = r; r += 262144; sink = r; r += 524288; sink = r;
  }
  if (n & 8)
    r += 1;
  return r;
}

/* CBZ/CBNZ shapes: `cmp rN,#0` + eq/ne fuses into one 16-bit forward branch,
 * which is committed irrevocably (it cannot be widened at patch time), so its
 * range must hold even after everything in between shrinks. */
__attribute__((noinline)) int zero_tests(int a, int b)
{
  int r = 0;
  if (a == 0)
    r += 1;
  if (b != 0)
    r += 2;
  if (a == 0 && b == 0)
    r += 4;
  return r;
}

/* Zero-test whose skipped body is long enough to sit near the 126-byte reach. */
__attribute__((noinline)) int zero_far(int a)
{
  int r = 0;
  if (a == 0)
  {
    r += 1; sink = r; r += 2; sink = r; r += 3; sink = r; r += 4; sink = r;
    r += 5; sink = r; r += 6; sink = r; r += 7; sink = r; r += 8; sink = r;
    r += 9; sink = r; r += 10; sink = r; r += 11; sink = r; r += 12; sink = r;
  }
  return r;
}

__attribute__((noinline)) int deep_nest(int a, int b, int c)
{
  int r = 0;
  if (a > 0)
  {
    if (b > 0)
    {
      if (c > 0)
        r = 1;
      else
        r = 2;
    }
    else
      r = 3;
  }
  else if (b > 0)
    r = 4;
  else if (c > 0)
    r = 5;
  else
    r = 6;
  return r;
}

/* Forward branch out of a loop: the exit target is far ahead of the test. */
__attribute__((noinline)) int loop_exit(const int *p, int n, int limit)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    if (p[i] > limit)
      break;
    s += p[i];
    sink = s;
  }
  return s;
}

int main(void)
{
  if (near_chain(3, 1) != 1 + 2 + 4 || near_chain(1, 3) != 1 + 2 ||
      near_chain(2, 2) != 1 + 2 + 8 || near_chain(-1, -1) != 8)
  {
    printf("FAIL near_chain %d %d %d %d\n", near_chain(3, 1), near_chain(1, 3),
           near_chain(2, 2), near_chain(-1, -1));
    return 1;
  }

  static const unsigned expect[8] = {
      0x12345678u + 0x4b5a6978u + 0xfeedfaceu + 0x89abcdefu + 0xaa55aa55u + 0x2468ace0u + 0xbaddcafeu + 0xf00dbabeu,
      0x9abcdef0u + 0x0f1e2d3cu + 0xfeedfaceu + 0x89abcdefu + 0xaa55aa55u + 0x2468ace0u + 0xbaddcafeu + 0xf00dbabeu,
      0x9abcdef0u + 0x4b5a6978u + 0xdeadbeefu + 0x89abcdefu + 0xaa55aa55u + 0x2468ace0u + 0xbaddcafeu + 0xf00dbabeu,
      0x9abcdef0u + 0x4b5a6978u + 0xfeedfaceu + 0x01234567u + 0xaa55aa55u + 0x2468ace0u + 0xbaddcafeu + 0xf00dbabeu,
      0x9abcdef0u + 0x4b5a6978u + 0xfeedfaceu + 0x89abcdefu + 0x55aa55aau + 0x2468ace0u + 0xbaddcafeu + 0xf00dbabeu,
      0x9abcdef0u + 0x4b5a6978u + 0xfeedfaceu + 0x89abcdefu + 0xaa55aa55u + 0x13579bdfu + 0xbaddcafeu + 0xf00dbabeu,
      0x9abcdef0u + 0x4b5a6978u + 0xfeedfaceu + 0x89abcdefu + 0xaa55aa55u + 0x2468ace0u + 0xcafebabeu + 0xf00dbabeu,
      0x9abcdef0u + 0x4b5a6978u + 0xfeedfaceu + 0x89abcdefu + 0xaa55aa55u + 0x2468ace0u + 0xbaddcafeu + 0x0badf00du,
  };
  for (int i = 0; i < 8; i++)
    if (pool_heavy(i) != expect[i])
    {
      printf("FAIL pool_heavy %d: %08x != %08x\n", i, pool_heavy(i), expect[i]);
      return 2;
    }

  if (spread(0) != 0 || spread(1) != 3 || spread(2) != 252 || spread(3) != 255 ||
      spread(4) != 1048320 || spread(15) != 1048576)
  {
    printf("FAIL spread %d %d %d %d %d %d\n", spread(0), spread(1), spread(2), spread(3),
           spread(4), spread(15));
    return 3;
  }

  if (deep_nest(1, 1, 1) != 1 || deep_nest(1, 1, -1) != 2 || deep_nest(1, -1, 0) != 3 ||
      deep_nest(-1, 1, 0) != 4 || deep_nest(-1, -1, 1) != 5 || deep_nest(-1, -1, -1) != 6)
  {
    printf("FAIL deep_nest\n");
    return 4;
  }

  if (zero_tests(0, 0) != 1 + 4 || zero_tests(0, 1) != 1 + 2 || zero_tests(1, 0) != 0 ||
      zero_tests(1, 1) != 2)
  {
    printf("FAIL zero_tests %d %d %d %d\n", zero_tests(0, 0), zero_tests(0, 1), zero_tests(1, 0),
           zero_tests(1, 1));
    return 6;
  }
  if (zero_far(0) != 78 || zero_far(5) != 0)
  {
    printf("FAIL zero_far %d %d\n", zero_far(0), zero_far(5));
    return 7;
  }

  static const int arr[6] = {1, 2, 3, 100, 4, 5};
  if (loop_exit(arr, 6, 50) != 6 || loop_exit(arr, 6, 1000) != 115 || loop_exit(arr, 0, 10) != 0)
  {
    printf("FAIL loop_exit %d %d %d\n", loop_exit(arr, 6, 50), loop_exit(arr, 6, 1000),
           loop_exit(arr, 0, 10));
    return 8;
  }

  printf("OK\n");
  return 0;
}
