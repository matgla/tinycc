/* cmp_imm_swap: a compare whose FIRST operand the optimizer folds to a
 * constant has to exchange its operands to reach `cmp`'s immediate slot, which
 * means mirroring the condition on every flag reader.  Get the mirror wrong and
 * the predicate silently inverts, so this walks each relation over the values
 * either side of the constant, the signed/unsigned split, and both INT
 * extremes (where the ARM condition codes rely on the V flag). */

#include <stdio.h>

#define MK(name, TYPE, OP)                          \
  int name(TYPE x)                                  \
  {                                                 \
    TYPE k = 0;                                     \
    for (int i = 0; i < 3; i++)                     \
      k = (TYPE)7;                                  \
    return (k OP x) ? 11 : 22;                      \
  }

MK(s_lt, int, <)
MK(s_le, int, <=)
MK(s_gt, int, >)
MK(s_ge, int, >=)
MK(s_eq, int, ==)
MK(s_ne, int, !=)
MK(u_lt, unsigned, <)
MK(u_le, unsigned, <=)
MK(u_gt, unsigned, >)
MK(u_ge, unsigned, >=)

/* The counted loop whose body turns out invariant: sccp folds the counter to
 * its initial value, leaving the guard as `0 < n` -- the shape every
 * `for (n = 0; n < iterations; n++)` benchmark head compiled to.  Reader is a
 * SELECT here and a JUMPIF in guard_branch. */
int guard(int n)
{
  int r = 0;
  for (int i = 0; i < n; i++)
    r = 1192;
  return r;
}

int guard_branch(int n)
{
  int r = 0;
  for (int i = 0; i < n; i++)
    r = 1192;
  if (r)
    return r + 1;
  return -1;
}

static const int vals[] = {0, 1, 6, 7, 8, -1, -7, 2147483647, -2147483647 - 1};

/* The guard probes run their loop for real at -O0, so they get small trip
 * counts; the INT extremes belong to the relational probes above, which are
 * O(1) at every level. */
static const int guard_vals[] = {0, 1, 6, -1};

int main(void)
{
  /* One packed line per probe value: printf with a dozen arguments nine times
   * over is slow enough at -O0 to trip the harness timeout under a parallel
   * run, and the bit-packed signature says just as much on a mismatch. */
  for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
  {
    int v = vals[i];
    unsigned sig = 0;
    sig |= (unsigned)(s_lt(v) == 11) << 0;
    sig |= (unsigned)(s_le(v) == 11) << 1;
    sig |= (unsigned)(s_gt(v) == 11) << 2;
    sig |= (unsigned)(s_ge(v) == 11) << 3;
    sig |= (unsigned)(s_eq(v) == 11) << 4;
    sig |= (unsigned)(s_ne(v) == 11) << 5;
    sig |= (unsigned)(u_lt((unsigned)v) == 11) << 6;
    sig |= (unsigned)(u_le((unsigned)v) == 11) << 7;
    sig |= (unsigned)(u_gt((unsigned)v) == 11) << 8;
    sig |= (unsigned)(u_ge((unsigned)v) == 11) << 9;
    printf("%d %03x\n", v, sig);
  }

  for (unsigned i = 0; i < sizeof(guard_vals) / sizeof(guard_vals[0]); i++)
  {
    int v = guard_vals[i];
    printf("g %d %d %d\n", v, guard(v), guard_branch(v));
  }
  return 0;
}
