/* Guard: the phase-3 scratch fixup may spill a blocking vreg to memory.
 *
 * When every allocatable register is occupied across a whole region, the dry
 * run's scratch borrow degenerates into save/use/restore at each instruction:
 * the backend stores the borrowed register into the scratch save area, loads
 * the address it needs, loads the value, then puts the borrowed register back.
 * gcc.c-torture 20041011-1 pays that at 20 of its 30 volatile copies — 7
 * instructions each instead of 2, plus a literal-pool reload of the global's
 * address every time, because the borrow invalidates the cached copy.
 *
 * try_demote_scratch_conflict() answers by demoting the blocker outright, so
 * its register becomes a permanently free scratch for the window.  That
 * rewrites the vreg's allocation (register -> stack slot) AFTER register
 * allocation has finished, and carves the slot out of the frame after the
 * dry run has settled — so it can go wrong in ways only execution catches:
 * a value read from the wrong slot, a slot that overlaps the outgoing-args or
 * scratch-save area, or a demoted vreg whose copy the coalescer had already
 * erased on the assumption that both ends share the register.
 *
 * Every case here therefore asserts values, under enough register pressure to
 * make the demotion fire.  Whether it fires is a code-size question, measured
 * by scripts/regression_disasm.py, not asserted here.
 */
#include <stdio.h>

volatile int gvol[32];

#define MULTI(X)                                                                                                       \
  X(1), X(2), X(3), X(4), X(5), X(6), X(7), X(8), X(9), X(10), X(11), X(12), X(13), X(14), X(15), X(16), X(17), X(18),  \
      X(19), X(20), X(21), X(22), X(23), X(24), X(25), X(26), X(27), X(28), X(29), X(30)

#define DECLARE(I) x##I
#define COPYIN(I) x##I = gvol[I]
#define COPYOUT(I) gvol[I] = x##I

/* The 20041011-1 shape: 30 live copies across a loop body, so the allocator
 * runs out of registers and the remaining copies borrow scratch. */
unsigned long long copy_loop(int n, unsigned long long x)
{
  while (n--)
  {
    int MULTI(DECLARE);
    MULTI(COPYIN);
    MULTI(COPYOUT);
    x += 3;
  }
  return x;
}

/* Same pressure, but the demoted value must survive a call: the slot must not
 * land in the outgoing-argument area, and the reload must see the stored
 * value and not a clobbered argument register. */
int sink(int a, int b, int c, int d, int e)
{
  return a + b + c + d + e;
}

int copy_loop_with_call(int n)
{
  int acc = 0;
  while (n--)
  {
    int MULTI(DECLARE);
    MULTI(COPYIN);
    acc += sink(x1, x2, x3, x4, x5);
    MULTI(COPYOUT);
    acc += x30 - x29;
  }
  return acc;
}

/* Pressure plus a value that is only defined on one arm of a diamond: a
 * demoted vreg whose def is conditional must still be stored on that path and
 * read back on the join. */
int copy_loop_diamond(int n, int sel)
{
  int acc = 0;
  while (n--)
  {
    int MULTI(DECLARE);
    MULTI(COPYIN);
    int pick = sel ? x7 * 3 : x8 + 5;
    MULTI(COPYOUT);
    acc += pick + x1;
  }
  return acc;
}

int main(void)
{
  int fails = 0;
  for (int i = 0; i < 32; i++)
    gvol[i] = i * 7 + 1;

  unsigned long long r = copy_loop(4, 0xffffffffULL);
  if (r != 0xffffffffULL + 12)
  {
    printf("copy_loop=%llu\n", r);
    fails++;
  }

  /* gvol must be unchanged: every element was copied out with its own value. */
  for (int i = 1; i <= 30; i++)
  {
    if (gvol[i] != i * 7 + 1)
    {
      printf("gvol[%d]=%d\n", i, gvol[i]);
      fails++;
    }
  }

  int c = copy_loop_with_call(3);
  int expect_call = 3 * ((1 * 7 + 1) + (2 * 7 + 1) + (3 * 7 + 1) + (4 * 7 + 1) + (5 * 7 + 1) + 7);
  if (c != expect_call)
  {
    printf("copy_loop_with_call=%d want %d\n", c, expect_call);
    fails++;
  }

  int d1 = copy_loop_diamond(2, 1);
  int expect_d1 = 2 * ((7 * 7 + 1) * 3 + (1 * 7 + 1));
  if (d1 != expect_d1)
  {
    printf("copy_loop_diamond(1)=%d want %d\n", d1, expect_d1);
    fails++;
  }

  int d0 = copy_loop_diamond(2, 0);
  int expect_d0 = 2 * ((8 * 7 + 1) + 5 + (1 * 7 + 1));
  if (d0 != expect_d0)
  {
    printf("copy_loop_diamond(0)=%d want %d\n", d0, expect_d0);
    fails++;
  }

  printf("fails=%d\n", fails);
  return 0;
}
