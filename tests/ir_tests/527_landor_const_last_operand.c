/* && / || whose LAST operand is a compile-time constant, after runtime
 * operands.  The constant is the operator's neutral element, so the result is
 * whatever the jump chain says -- but && used to read it off a synthetic
 * compare with no CMP behind it, taking the flags the previous operand's
 * test left.  After `!x` (a jump on NE) those flags say EQ on the
 * fallthrough, and `a && !x && 1` came out 0.  The self-hosted tcc hit it in
 * frame_record's `guard = ir && optimize > 0 && !do_debug &&
 * !tcc_bounds_checking(s)`, with tcc_bounds_checking the constant 0.
 *
 * Each case is used both as a value and as a branch condition, at every
 * combination of its runtime operands. */
#include <stdio.h>

#define BCHECK(s) 0

struct st
{
  void *ir;
  signed char optimize;
  unsigned char do_debug;
};

__attribute__((noinline)) int guard_of(struct st *s, int size, int mask)
{
  void *ir = s ? s->ir : NULL;
  int guard = ir && s->optimize > 0 && !s->do_debug && !BCHECK(s);
  if (guard && size >= 4)
    mask &= -4;
  return mask + guard * 100;
}

__attribute__((noinline)) int and_not_one(int a, int x)
{
  return a && !x && 1;
}

__attribute__((noinline)) int and_cmp_one(int a, int x)
{
  return a > 0 && x != 3 && 1;
}

__attribute__((noinline)) int and_branch(int a, int x)
{
  if (a && !x && 1)
    return 7;
  return 9;
}

__attribute__((noinline)) int or_not_zero(int a, int x)
{
  return a || !x || 0;
}

__attribute__((noinline)) int or_branch(int a, int x)
{
  if (a || !x || 0)
    return 7;
  return 9;
}

__attribute__((noinline)) int and_value_store(volatile int *out, int a, int x)
{
  int v = !a && !x && 1;
  *out = v;
  return v ? 11 : 13;
}

int main(void)
{
  static struct st st = {(void *)&st, 2, 0};
  int fails = 0;

  int r = guard_of(&st, 8, -1);
  printf("guard %d\n", r);
  fails += r != 96;
  st.do_debug = 1;
  fails += guard_of(&st, 8, -1) != -1;
  st.do_debug = 0;
  st.optimize = 0;
  fails += guard_of(&st, 8, -1) != -1;
  fails += guard_of(NULL, 8, -1) != -1;

  for (int a = 0; a < 2; a++)
    for (int x = 0; x < 2; x++)
    {
      volatile int out = -1;
      int e_and = a && !x, e_or = a || !x, e_nn = !a && !x;
      fails += and_not_one(a, x) != e_and;
      fails += and_branch(a, x) != (e_and ? 7 : 9);
      fails += or_not_zero(a, x) != e_or;
      fails += or_branch(a, x) != (e_or ? 7 : 9);
      fails += and_value_store(&out, a, x) != (e_nn ? 11 : 13);
      fails += out != e_nn;
      printf("a=%d x=%d and=%d or=%d\n", a, x, and_not_one(a, x), or_not_zero(a, x));
    }
  for (int a = -1; a < 2; a++)
    for (int x = 2; x < 5; x++)
      fails += and_cmp_one(a, x) != (a > 0 && x != 3);

  printf("fails %d\n", fails);
  return fails;
}
