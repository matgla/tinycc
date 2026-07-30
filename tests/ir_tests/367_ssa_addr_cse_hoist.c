/* ssa:local_addr_cse + ssa:loop_addr_hoist — materialize a global address once
 * per straight-line region / once per loop instead of at every symref operand.
 *
 * Three shapes are pinned here, all for CORRECTNESS (the rewritten operands
 * must still address the same globals):
 *
 *  - `straight`: several uses of &g_a and &g_b inside one call-free region,
 *    which local_addr_cse collapses onto two temps.
 *  - `looped`: &g_arr used inside a loop, which loop_addr_hoist moves to the
 *    preheader; the loop must still read the array, not a stale register.
 *  - `weaved`: a goto INTO a loop body, so the loop is irreducible and the
 *    preheader does not dominate the body.  Hoisting there would leave the
 *    address temp undefined on the goto edge — the pass must decline.
 *    (tests2/87_dead_code caught this the hard way.)
 */
#include <stdio.h>

int g_a = 3;
int g_b = 5;
int g_arr[16];

static int straight(void)
{
  int s = g_a + g_b;
  s += g_a * 2;
  s += g_b * 4;
  g_a = s & 7;
  g_b = s & 3;
  return s + g_a + g_b;
}

static int looped(void)
{
  int s = 0;
  for (int i = 0; i < 16; i++)
    s += g_arr[i] * (i + 1);
  return s;
}

static int weaved(int n)
{
  int s = 0;
  int i = 0;
  int once = 1;
  if (n > 100) {
    while (i < 4) {
    inner:
      s += g_arr[i];
      i++;
    }
  }
  if (n == 7 && once) {
    once = 0;
    goto inner;
  }
  return s;
}

int main(void)
{
  for (int i = 0; i < 16; i++)
    g_arr[i] = i * 2 + 1;
  printf("straight=%d\n", straight());
  printf("looped=%d\n", looped());
  printf("weaved=%d\n", weaved(7));
  printf("g_a=%d g_b=%d\n", g_a, g_b);
  return 0;
}
