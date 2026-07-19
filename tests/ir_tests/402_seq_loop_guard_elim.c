/* Guard: zero-trip entry-guard elimination across sequential counted loops
 * (source/opt/flat/loop/seq_guard_elim.c + the rotation gate that depends on
 * it).  The pass carries an IV constant forward as exit = init + trip*step and
 * then NOPs a rotated loop's pre-loop `CMP iv,#lim / JUMPIF` — both the carry
 * and the guard proof must be exact, and every shape it declines must keep
 * running the loop it guards.
 *
 * Shapes pinned here:
 *   - the gcc-torture memclr chain: three loops sharing one `i`, each body
 *     ending in a noreturn call, every guard provably untaken;
 *   - a chain whose middle guard IS taken (zero-trip): dropping it would run
 *     the loop once too often;
 *   - a runtime entry value: nothing may be proven, the guards must stay;
 *   - an address-taken IV a call can change under the walker's feet;
 *   - a chain interrupted by a branch, so the carried value must be forgotten.
 */
#include <stdio.h>

static unsigned char buf[88];

/* The memclr shape.  Every guard is provably untaken (0<10, 10<54, 54<88), so
 * all three loops rotate and lose their guard; a wrong exit-value carry shows
 * up as a wrong count or a spurious abort. */
__attribute__((noinline)) int memclr_shape(void)
{
  int i, bad = 0;
  for (i = 0; i < 10; i++)
    if (buf[i] != 0xaa)
      bad++;
  for (; i < 54; i++)
    if (buf[i] != 0x00)
      bad++;
  for (; i < 88; i++)
    if (buf[i] != 0xaa)
      bad++;
  return bad * 1000 + i;
}

/* Middle loop is zero-trip: its guard IS taken, so it must survive.  `i`
 * leaves loop 1 at 10, loop 2 must not run at all, loop 3 runs 10..20. */
__attribute__((noinline)) int zero_trip_middle(void)
{
  int i, n1 = 0, n2 = 0, n3 = 0;
  for (i = 0; i < 10; i++)
    n1++;
  for (; i < 5; i++)
    n2++;
  for (; i < 20; i++)
    n3++;
  return n1 * 10000 + n2 * 100 + n3 * 1 + i * 1000000;
}

/* Entry value is not a constant: no guard may be removed, and the first loop
 * must still be skipped entirely when start >= 10. */
__attribute__((noinline)) int runtime_entry(int start)
{
  int i, n1 = 0, n2 = 0;
  for (i = start; i < 10; i++)
    n1++;
  for (; i < 20; i++)
    n2++;
  return n1 * 1000 + n2 + i * 1000000;
}

static int bump(int *p) { *p = 30; return 0; }

/* The IV's address escapes, so the call in the first body can move it: the
 * walker must not carry a closed-form exit value past that loop. */
__attribute__((noinline)) int addrtaken_iv(void)
{
  int i, n1 = 0, n2 = 0;
  for (i = 0; i < 10; i++)
  {
    n1++;
    if (i == 4)
      bump(&i);
  }
  for (; i < 40; i++)
    n2++;
  return n1 * 1000 + n2 + i * 1000000;
}

/* A branch between the loops joins two different IV values, so the second
 * chain's guard cannot be proven from the first loop's exit value. */
__attribute__((noinline)) int branch_between(int sel)
{
  int i, n = 0;
  for (i = 0; i < 10; i++)
    n++;
  if (sel)
    i = 25;
  for (; i < 20; i++)
    n++;
  return n * 1000 + i;
}

/* Negative step: the closed form must not be applied with the wrong sign. */
__attribute__((noinline)) int down_chain(void)
{
  int i, n1 = 0, n2 = 0;
  for (i = 20; i > 10; i--)
    n1++;
  for (; i > 4; i--)
    n2++;
  return n1 * 1000 + n2 + i * 1000000;
}

int main(void)
{
  int i;
  for (i = 0; i < 88; i++)
    buf[i] = (i >= 10 && i < 54) ? 0x00 : 0xaa;

  printf("memclr_shape: %d\n", memclr_shape());
  printf("zero_trip_middle: %d\n", zero_trip_middle());
  printf("runtime_entry: %d %d %d\n", runtime_entry(0), runtime_entry(7),
         runtime_entry(15));
  printf("addrtaken_iv: %d\n", addrtaken_iv());
  printf("branch_between: %d %d\n", branch_between(0), branch_between(1));
  printf("down_chain: %d\n", down_chain());

  /* A wrong carry only shows up when the data disagrees, so re-run the memclr
   * shape over a buffer with one poisoned byte in each region. */
  buf[3] = 0;
  buf[20] = 0xaa;
  buf[80] = 0;
  printf("memclr_shape poisoned: %d\n", memclr_shape());
  return 0;
}
