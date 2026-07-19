/* Firing shapes for return-constant register reuse: a `return C;` whose
 * block is entered only through the equality edge of `TEST_ZERO V` (C == 0)
 * or `CMP V, #C` must return V (which provably equals C on that edge and
 * already sits in a register) instead of rematerializing C.
 *
 * SSA home: ssa:branch (return-const reuse gen).  The retired flat
 * return_reuse pass did the same rewrite pre-SSA.  Mirrors the gcc-torture
 * shapes pr106433.c::bar (TEST_ZERO, C == 0) and 920812-1.c::f (CMP, C == 1).
 */

int m, *p;

/* TEST_ZERO x; the x==0 (EQ) edge returns 0: r0 already holds 0 there, so
 * no `movs r0, #0` may be emitted on that path.  The infinite loop keeps
 * if-conversion from absorbing the diamond. */
int bar_inf(int x)
{
  if (x)
    {
      p = &x;
      for (;;)
        ++m;
    }
  return 0;
}

/* CMP y,#1; the y==1 (EQ) edge returns 1: r0 already holds 1 there, so no
 * `movs r0, #1` may be emitted. */
typedef int t;
int f_switch(t y)
{
  switch (y)
    {
    case 1:
      return 1;
    }
  return 0;
}
